#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <SD_MMC.h>
#include <CSV_Parser.h>
#include <Wire.h>
#include <ArtronShop_RX8130CE.h>
#include <esp_sntp.h>
#include <algorithm>

//==================== 定数・マクロ ====================
const char* SCHEDULE_FILE = "/schedule.csv";

// Tab5 RTC設定
ArtronShop_RX8130CE rtc(&Wire1);
bool rtc_ok = false;
bool sdcard_ok = false;
bool wifi_ok = false;
bool standalone_mode = false;
int future_scroll_offset = 0;
int future_max_offset    = 0;

const int SCREEN_WIDTH = 1280;
const int SCREEN_HEIGHT = 720;

// 全画面のオフスクリーンバッファ。描画はすべてここへ行い、変更した矩形だけを pushArea() で転送する。
// Tab5 は PSRAM 上の単一フレームバッファを DSI が常時読み出しているため、
// ディスプレイへの細切れの直接描画は帯域を圧迫して画面全体の色化け（水色のフラッシュ）を起こす。
M5Canvas canvas(&M5.Display);

struct Schedule {
  time_t start;
  time_t stop;
  String action;
};
std::vector<Schedule> schedules;

//--- シリアル時刻設定用のバッファ ---
String serialBuffer = "";

void handleSerialTimeSet();

//==================== テーマ ====================
// uint32_t の色は RGB888 として扱われる
constexpr uint32_t C_BG        = 0xF1F5F9;
constexpr uint32_t C_CARD      = 0xFFFFFF;
constexpr uint32_t C_BORDER    = 0xE2E8F0;
constexpr uint32_t C_SHADOW    = 0xDCE3EB;
constexpr uint32_t C_TEXT      = 0x0F172A;
constexpr uint32_t C_SUB       = 0x64748B;
constexpr uint32_t C_MUTED     = 0xCBD5E1;
constexpr uint32_t C_ACCENT    = 0x2563EB;
constexpr uint32_t C_ACCENT_DK = 0x1D4ED8;
constexpr uint32_t C_ACCENT_BG = 0xEFF6FF;
constexpr uint32_t C_LIVE      = 0x16A34A;
constexpr uint32_t C_LIVE_BG   = 0xF0FDF4;
constexpr uint32_t C_WARN      = 0xD97706;
constexpr uint32_t C_ERR       = 0xDC2626;
constexpr uint32_t C_WHITE     = 0xFFFFFF;

const lgfx::IFont* const F_JP24 = &fonts::lgfxJapanGothicP_24;
const lgfx::IFont* const F_JP28 = &fonts::lgfxJapanGothicP_28;
const lgfx::IFont* const F_JP32 = &fonts::lgfxJapanGothicP_32;
const lgfx::IFont* const F_JP40 = &fonts::lgfxJapanGothicP_40;
const lgfx::IFont* const F_NUM  = &fonts::DejaVu72;   // 数字の高さ 52px

using Datum = lgfx::textdatum::textdatum_t;

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py, int pad = 0) const {
    return px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad;
  }
};

const int CARD_RADIUS = 20;
const int CARD_SHADOW = 4;   // カードの影の分だけ下に広げて消去・転送する

//--- メイン画面レイアウト ---
const Rect R_HEADER     = {0, 0, SCREEN_WIDTH, 56};
const Rect R_CLOCK      = {24, 64, 1232, 272};
const Rect R_BOTTOM     = {24, 352, 1232, 344};
const Rect R_ONGOING    = {24, 352, 792, 344};
const Rect R_FUTURE     = {832, 352, 424, 344};
const Rect BTN_TIME_SET = {1088, 8, 168, 40};
const Rect BTN_UP       = {1114, 366, 52, 52};
const Rect BTN_DOWN     = {1180, 366, 52, 52};

const int   CLOCK_X    = R_CLOCK.x + 48;    // HH:MM の左端
const int   CLOCK_BASE = R_CLOCK.y + 214;   // 時計数字のベースライン
const float CLOCK_SIZE = 3.0f;
const float SEC_SIZE   = 1.5f;
const int   ONGOING_VISIBLE = 3;
const int   FUTURE_VISIBLE  = 2;

const char* const WDAY_JA[] = { "日", "月", "火", "水", "木", "金", "土" };

//==================== 描画ヘルパー ====================
//--- 指定矩形だけをディスプレイへ転送（1回の書き込みトランザクションにまとめる）---
static void pushArea(int x, int y, int w, int h) {
  M5.Display.startWrite();
  M5.Display.setClipRect(x, y, w, h);
  canvas.pushSprite(0, 0);
  M5.Display.clearClipRect();
  M5.Display.endWrite();
}

static void pushCardArea(const Rect& r) {
  pushArea(r.x, r.y, r.w, r.h + CARD_SHADOW);
}

static void clearCardArea(const Rect& r) {
  canvas.fillRect(r.x, r.y, r.w, r.h + CARD_SHADOW, C_BG);
}

static int drawText(const String& s, int x, int y, const lgfx::IFont* font, uint32_t color,
                    Datum datum, float size = 1.0f) {
  canvas.setFont(font);
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setTextDatum(datum);
  return canvas.drawString(s.c_str(), x, y);
}

//--- 幅に収まらない文字列を UTF-8 文字単位で削って「…」を付ける ---
static String fitText(const String& s, int maxW, const lgfx::IFont* font, float size = 1.0f) {
  canvas.setFont(font);
  canvas.setTextSize(size);
  if (canvas.textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 0) {
    int i = t.length() - 1;
    while (i > 0 && ((uint8_t)t[i] & 0xC0) == 0x80) i--;
    t.remove(i);
    String c = t + "…";
    if (canvas.textWidth(c) <= maxW) return c;
  }
  return String("…");
}

static void drawCard(const Rect& r, uint32_t fill = C_CARD, uint32_t border = C_BORDER, int bw = 1) {
  canvas.fillSmoothRoundRect(r.x, r.y + 2, r.w, r.h, CARD_RADIUS, C_SHADOW);
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, CARD_RADIUS, border);
  canvas.fillSmoothRoundRect(r.x + bw, r.y + bw, r.w - bw * 2, r.h - bw * 2, CARD_RADIUS - bw, fill);
}

static void drawPillButton(const Rect& b, const String& label, uint32_t fill, uint32_t fg,
                           const lgfx::IFont* font, uint32_t border) {
  int rad = b.h / 2;
  canvas.fillSmoothRoundRect(b.x, b.y, b.w, b.h, rad, border);
  canvas.fillSmoothRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, rad - 1, fill);
  if (label.length() > 0) {
    drawText(label, b.x + b.w / 2, b.y + b.h / 2, font, fg, Datum::middle_center);
  }
}

static void drawProgress(int x, int y, int w, int h, float p, uint32_t color) {
  p = constrain(p, 0.0f, 1.0f);
  canvas.fillSmoothRoundRect(x, y, w, h, h / 2, C_BORDER);
  canvas.fillSmoothRoundRect(x, y, std::max(h, (int)(w * p)), h, h / 2, color);
}

static void drawAlertIcon(int cx, int cy, int r, uint32_t color) {
  canvas.fillSmoothCircle(cx, cy, r, color);
  int bw = std::max(4, r / 5);
  canvas.fillSmoothRoundRect(cx - bw / 2, cy - r * 3 / 5, bw, r * 3 / 4, bw / 2, C_WHITE);
  canvas.fillSmoothCircle(cx, cy + r / 2, std::max(2, bw / 2 + 1), C_WHITE);
}

static void drawArrowButton(const Rect& b, bool up, bool enabled) {
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  canvas.fillSmoothCircle(cx, cy, b.w / 2, enabled ? C_ACCENT : C_BORDER);
  uint32_t c = enabled ? C_WHITE : C_MUTED;
  int d = up ? -1 : 1;
  canvas.drawWideLine(cx - 10, cy - 5 * d, cx, cy + 5 * d, 2.5f, c);
  canvas.drawWideLine(cx, cy + 5 * d, cx + 10, cy - 5 * d, 2.5f, c);
}

//==================== 時刻フォーマット ====================
//--- 時刻表示用フォーマット ---
String formatDateTime(struct tm &t) {
  char buffer[32];
  sprintf(buffer, "%04d/%02d/%02d %02d:%02d:%02d",
    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
    t.tm_hour, t.tm_min, t.tm_sec);
  return String(buffer);
}

//--- "YYYY/M/D H:MM" → time_t ---
time_t parseDateTime(const String& dt) {
  int y, M, d, h, m;
  sscanf(dt.c_str(), "%d/%d/%d %d:%d", &y, &M, &d, &h, &m);
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon  = M - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = m;
  t.tm_sec  = 0;
  return mktime(&t);
}

static struct tm toLocal(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return tm;
}

static String formatHM(time_t t) {
  struct tm tm = toLocal(t);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

static String formatRange(const Schedule& s) {
  return formatHM(s.start) + "～" + formatHM(s.stop);
}

static String formatMonthDay(time_t t) {
  struct tm tm = toLocal(t);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%d(%s)", tm.tm_mon + 1, tm.tm_mday, WDAY_JA[tm.tm_wday]);
  return String(buf);
}

static bool isSameDay(time_t a, time_t b) {
  struct tm ta = toLocal(a), tb = toLocal(b);
  return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

//--- 残り時間などの表示（分単位で切り上げ）---
static String formatDuration(time_t sec) {
  long m = (sec + 59) / 60;
  if (m >= 24 * 60) {
    long h = (m % (24 * 60)) / 60;
    return String(m / (24 * 60)) + "日" + (h ? String(h) + "時間" : String(""));
  }
  if (m >= 60) {
    return String(m / 60) + "時間" + (m % 60 ? String(m % 60) + "分" : String(""));
  }
  return String(m) + "分";
}

//--- RTC時刻が有効（2020年以降）かチェック ---
bool isRTCTimeValid() {
  if (!rtc_ok) return false;
  struct tm t;
  if (!rtc.getTime(&t)) return false;
  return (t.tm_year + 1900) >= 2020;
}

//==================== 起動画面 ====================
enum BootState : uint8_t { BOOT_PENDING, BOOT_RUNNING, BOOT_OK, BOOT_WARN, BOOT_FAIL, BOOT_SKIP };
enum BootStep  : uint8_t { STEP_RTC, STEP_WIFI, STEP_NTP, STEP_SD, STEP_SCHEDULE, STEP_COUNT };

const char* const BOOT_LABELS[STEP_COUNT] = { "RTC", "Wi-Fi", "NTP時刻同期", "SDカード", "スケジュール" };
BootState bootStates[STEP_COUNT] = {};
String    bootDetails[STEP_COUNT];
String    bootHint = "";
const Rect R_BOOT = {240, 80, 800, 560};

static void drawStatusIcon(int cx, int cy, BootState st) {
  const int r = 14;
  switch (st) {
    case BOOT_PENDING:
      canvas.fillSmoothCircle(cx, cy, r, C_BORDER);
      canvas.fillSmoothCircle(cx, cy, r - 3, C_CARD);
      break;
    case BOOT_RUNNING:
      canvas.fillSmoothCircle(cx, cy, r, C_ACCENT);
      canvas.fillSmoothCircle(cx, cy, r - 3, C_CARD);
      canvas.fillSmoothCircle(cx, cy, 5, C_ACCENT);
      break;
    case BOOT_OK:
      canvas.fillSmoothCircle(cx, cy, r, C_LIVE);
      canvas.drawWideLine(cx - 6, cy, cx - 2, cy + 5, 2.0f, C_WHITE);
      canvas.drawWideLine(cx - 2, cy + 5, cx + 6, cy - 5, 2.0f, C_WHITE);
      break;
    case BOOT_WARN:
      drawAlertIcon(cx, cy, r, C_WARN);
      break;
    case BOOT_FAIL:
      canvas.fillSmoothCircle(cx, cy, r, C_ERR);
      canvas.drawWideLine(cx - 5, cy - 5, cx + 5, cy + 5, 2.0f, C_WHITE);
      canvas.drawWideLine(cx - 5, cy + 5, cx + 5, cy - 5, 2.0f, C_WHITE);
      break;
    case BOOT_SKIP:
      canvas.fillSmoothCircle(cx, cy, r, C_MUTED);
      canvas.drawWideLine(cx - 6, cy, cx + 6, cy, 2.0f, C_WHITE);
      break;
  }
}

static void renderBootCard() {
  clearCardArea(R_BOOT);
  drawCard(R_BOOT);
  int cx = R_BOOT.x + R_BOOT.w / 2;
  drawText("Tab5 スケジュール", cx, R_BOOT.y + 64, F_JP40, C_TEXT, Datum::middle_center);
  drawText("初期化しています…", cx, R_BOOT.y + 110, F_JP24, C_SUB, Datum::middle_center);

  const int left = R_BOOT.x + 64, right = R_BOOT.x + R_BOOT.w - 64;
  for (int i = 0; i < STEP_COUNT; i++) {
    int y = R_BOOT.y + 180 + i * 64;
    if (i > 0) canvas.drawFastHLine(left, y - 32, right - left, C_BORDER);
    BootState st = bootStates[i];
    drawStatusIcon(left + 16, y, st);
    drawText(BOOT_LABELS[i], left + 48, y, F_JP32, st == BOOT_PENDING ? C_SUB : C_TEXT, Datum::middle_left);
    uint32_t dc = (st == BOOT_FAIL) ? C_ERR : (st == BOOT_WARN) ? C_WARN : (st == BOOT_RUNNING) ? C_ACCENT : C_SUB;
    drawText(fitText(bootDetails[i], 380, F_JP24), right, y, F_JP24, dc, Datum::middle_right);
  }
  if (bootHint.length() > 0) {
    drawText(fitText(bootHint, R_BOOT.w - 64, F_JP24), cx, R_BOOT.y + R_BOOT.h - 48, F_JP24, C_SUB, Datum::middle_center);
  }
}

static void showBootScreen() {
  canvas.fillScreen(C_BG);
  renderBootCard();
  pushArea(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

static void bootStep(BootStep step, BootState st, const String& detail = "") {
  bootStates[step]  = st;
  bootDetails[step] = detail;
  renderBootCard();
  pushCardArea(R_BOOT);
}

//==================== 起動モード選択画面 ====================
static void drawWifiIcon(int cx, int cy, uint32_t c) {
  int by = cy + 20;
  canvas.fillArc(cx, by, 36, 44, 225, 315, c);
  canvas.fillArc(cx, by, 20, 28, 225, 315, c);
  canvas.fillSmoothCircle(cx, by - 2, 6, c);
}

static void drawClockIcon(int cx, int cy, uint32_t c, uint32_t bg) {
  canvas.fillSmoothCircle(cx, cy, 32, c);
  canvas.fillSmoothCircle(cx, cy, 26, bg);
  canvas.drawWideLine(cx, cy, cx, cy - 16, 3.0f, c);
  canvas.drawWideLine(cx, cy, cx + 12, cy + 6, 3.0f, c);
}

//--- 起動モード選択画面（5秒でスタンドアロンへ自動移行）---
void showModeSelectionScreen() {
  const Rect cardWifi = {160, 170, 460, 300};
  const Rect cardSa   = {660, 170, 460, 300};
  const Rect rCount   = {240, 510, 800, 80};
  const uint32_t TIMEOUT_MS = 5000;

  auto renderCard = [&](const Rect& r, bool wifi, bool pressed) {
    uint32_t accent = wifi ? C_ACCENT : C_LIVE;
    uint32_t tint   = wifi ? C_ACCENT_BG : C_LIVE_BG;
    clearCardArea(r);
    if (pressed)   drawCard(r, tint, accent, 4);
    else if (!wifi) drawCard(r, C_CARD, accent, 3);   // タイムアウト時の既定
    else           drawCard(r);
    int cx = r.x + r.w / 2, iy = r.y + 92;
    uint32_t iconBg = pressed ? C_WHITE : tint;
    canvas.fillSmoothCircle(cx, iy, 52, iconBg);
    if (wifi) drawWifiIcon(cx, iy, accent);
    else      drawClockIcon(cx, iy, accent, iconBg);
    drawText(wifi ? "Wi-Fi接続" : "スタンドアロン", cx, r.y + 186, F_JP40, C_TEXT, Datum::middle_center);
    drawText(wifi ? "NTPで時刻を自動同期" : "内蔵RTCの時刻で起動", cx, r.y + 238, F_JP28, C_SUB, Datum::middle_center);
  };

  auto renderCountdown = [&](uint32_t remainMs) {
    canvas.fillRect(rCount.x, rCount.y, rCount.w, rCount.h, C_BG);
    int sec = (remainMs + 999) / 1000;
    drawText(String(sec) + " 秒後にスタンドアロンモードで起動します", SCREEN_WIDTH / 2, rCount.y + 20,
             F_JP28, C_SUB, Datum::middle_center);
    drawProgress(rCount.x + 100, rCount.y + 56, rCount.w - 200, 8, (float)remainMs / TIMEOUT_MS, C_LIVE);
  };

  canvas.fillScreen(C_BG);
  drawText("起動モードを選択", SCREEN_WIDTH / 2, 80, F_JP40, C_TEXT, Datum::middle_center);
  drawText("タップして選択してください", SCREEN_WIDTH / 2, 128, F_JP28, C_SUB, Datum::middle_center);
  renderCard(cardWifi, true, false);
  renderCard(cardSa, false, false);
  renderCountdown(TIMEOUT_MS);
  pushArea(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

  uint32_t start = millis(), lastDraw = start;
  while (millis() - start < TIMEOUT_MS) {
    handleSerialTimeSet();
    M5.update();
    auto td = M5.Touch.getDetail();
    if (td.wasPressed()) {
      bool wifi = cardWifi.contains(td.x, td.y);
      if (wifi || cardSa.contains(td.x, td.y)) {
        const Rect& r = wifi ? cardWifi : cardSa;
        renderCard(r, wifi, true);
        pushCardArea(r);
        standalone_mode = !wifi;
        delay(300);
        return;
      }
    }
    if (millis() - lastDraw >= 100) {
      lastDraw = millis();
      renderCountdown(TIMEOUT_MS - std::min<uint32_t>(TIMEOUT_MS, lastDraw - start));
      pushArea(rCount.x, rCount.y, rCount.w, rCount.h);
    }
    delay(10);
  }
  standalone_mode = true;  // タイムアウト → スタンドアロン
}

//==================== 時刻設定画面 ====================
static void drawStepButton(const Rect& b, bool plus, bool pressed) {
  uint32_t fill = pressed ? C_ACCENT : C_BG;
  uint32_t fg   = pressed ? C_WHITE : C_TEXT;
  drawPillButton(b, "", fill, fg, F_JP24, pressed ? C_ACCENT : C_BORDER);
  int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  canvas.drawWideLine(cx - 14, cy, cx + 14, cy, 2.5f, fg);
  if (plus) canvas.drawWideLine(cx, cy - 14, cx, cy + 14, 2.5f, fg);
}

//--- タッチ操作による時刻設定画面 ---
void showTimeSetScreen() {
  struct tm t = {};
  if (rtc_ok) {
    rtc.getTime(&t);
  } else {
    t.tm_year = 2025 - 1900;
    t.tm_mon  = 5;
    t.tm_mday = 1;
    t.tm_hour = 0;
    t.tm_min  = 0;
    t.tm_sec  = 0;
  }

  const char* labels[] = { "年", "月", "日", "時", "分", "秒" };
  int* fields[] = { &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec };
  int mins[]    = { 100, 0, 1, 0, 0, 0 };
  int maxs[]    = { 200, 11, 31, 23, 59, 59 };

  const Rect rPreview = {560, 24, 680, 72};
  const Rect btnBack  = {288, 628, 240, 72};
  const Rect btnOk    = {552, 628, 440, 72};
  const char* OK_LABEL = "確定して RTC に保存";
  Rect cards[6], btnMinus[6], btnPlus[6];
  for (int i = 0; i < 6; i++) {
    cards[i]    = { 64 + (i % 3) * 392, 140 + (i / 3) * 236, 368, 212 };
    btnMinus[i] = { cards[i].x + 20,  cards[i].y + 136, 156, 60 };
    btnPlus[i]  = { cards[i].x + 192, cards[i].y + 136, 156, 60 };
  }

  auto renderPreview = [&]() {
    canvas.fillRect(rPreview.x, rPreview.y, rPreview.w, rPreview.h, C_BG);
    struct tm n = t;
    n.tm_isdst = -1;
    mktime(&n);  // 曜日の算出用
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d年%d月%d日（%s） %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WDAY_JA[n.tm_wday], t.tm_hour, t.tm_min, t.tm_sec);
    drawText(buf, rPreview.x + rPreview.w, rPreview.y + rPreview.h / 2, F_JP40, C_TEXT, Datum::middle_right);
  };

  // pressed: -1=なし, 0=−, 1=＋
  auto renderStepper = [&](int i, int pressed) {
    const Rect& c = cards[i];
    clearCardArea(c);
    drawCard(c);
    drawText(labels[i], c.x + 24, c.y + 20, F_JP28, C_SUB, Datum::top_left);
    int dispVal = (i == 0) ? *fields[i] + 1900 : (i == 1) ? *fields[i] + 1 : *fields[i];
    char valBuf[8];
    snprintf(valBuf, sizeof(valBuf), (i == 0) ? "%04d" : "%02d", dispVal);
    drawText(valBuf, c.x + c.w / 2, c.y + 80, F_NUM, C_TEXT, Datum::middle_center);
    drawStepButton(btnMinus[i], false, pressed == 0);
    drawStepButton(btnPlus[i],  true,  pressed == 1);
  };

  auto refresh = [&](int i, int pressed) {
    renderStepper(i, pressed);
    renderPreview();
    pushCardArea(cards[i]);
    pushArea(rPreview.x, rPreview.y, rPreview.w, rPreview.h);
  };

  auto step = [&](int i, int dir) -> bool {
    if (dir < 0 && *fields[i] > mins[i]) { (*fields[i])--; return true; }
    if (dir > 0 && *fields[i] < maxs[i]) { (*fields[i])++; return true; }
    return false;
  };

  canvas.fillScreen(C_BG);
  drawText("時刻設定", 64, 60, F_JP40, C_TEXT, Datum::middle_left);
  renderPreview();
  for (int i = 0; i < 6; i++) renderStepper(i, -1);
  drawPillButton(btnBack, "戻る", C_CARD, C_TEXT, F_JP32, C_BORDER);
  drawPillButton(btnOk, OK_LABEL, C_ACCENT, C_WHITE, F_JP32, C_ACCENT);
  pushArea(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

  // ボタン長押しで連続増減
  int heldField = -1, heldDir = 0;
  uint32_t holdStart = 0, lastRepeat = 0;

  while (true) {
    handleSerialTimeSet();
    M5.update();
    auto td = M5.Touch.getDetail();

    if (td.wasPressed()) {
      // 保存せずに戻る
      if (btnBack.contains(td.x, td.y)) {
        drawPillButton(btnBack, "戻る", C_BG, C_TEXT, F_JP32, C_MUTED);
        pushArea(btnBack.x, btnBack.y, btnBack.w, btnBack.h);
        Serial.println("時刻設定をキャンセルしました");
        delay(200);
        break;
      }
      if (btnOk.contains(td.x, td.y)) {
        drawPillButton(btnOk, OK_LABEL, C_ACCENT_DK, C_WHITE, F_JP32, C_ACCENT_DK);
        pushArea(btnOk.x, btnOk.y, btnOk.w, btnOk.h);

        t.tm_isdst = -1;
        if (rtc_ok) {
          rtc.setTime(t);
          Serial.println("RTC時刻を設定しました: " + formatDateTime(t));
        }
        time_t ts = mktime(&t);
        struct timeval tv = { ts, 0 };
        settimeofday(&tv, nullptr);
        delay(200);
        break;
      }
      for (int i = 0; i < 6; i++) {
        int dir = btnMinus[i].contains(td.x, td.y, 8) ? -1
                : btnPlus[i].contains(td.x, td.y, 8)  ?  1 : 0;
        if (dir != 0) {
          step(i, dir);
          heldField = i;
          heldDir   = dir;
          holdStart = lastRepeat = millis();
          refresh(i, dir < 0 ? 0 : 1);
          break;
        }
      }
    } else if (heldField >= 0) {
      if (td.isPressed()) {
        uint32_t now = millis();
        if (now - holdStart >= 500 && now - lastRepeat >= 100) {
          lastRepeat = now;
          if (step(heldField, heldDir)) refresh(heldField, heldDir < 0 ? 0 : 1);
        }
      } else {
        renderStepper(heldField, -1);
        pushCardArea(cards[heldField]);
        heldField = -1;
      }
    }
    delay(15);
  }
}

//==================== 初期化処理 ====================
//--- RTCの初期化 ---
void initRTC() {
  Serial.println("RTC初期化開始...");

  // Tab5のI2Cピン設定
  Wire1.begin(31, 32);  // SDA=31, SCL=32 (Wire1=I2C1, Wire0はタッチコントローラー用に確保)

  if (!rtc.begin()) {
    Serial.println("RTC初期化失敗 - RX8130CEが見つかりません");
    rtc_ok = false;
    return;
  }

  rtc_ok = true;
  Serial.println("RTC初期化成功");

  // 現在のRTC時刻を表示
  struct tm t;
  if (rtc.getTime(&t)) {
    Serial.println("現在のRTC時刻: " + formatDateTime(t));
  } else {
    Serial.println("RTC時刻取得エラー");
  }

  Serial.println("シリアルで時刻設定: YYYY/MM/DD HH:MM:SS");
}

//--- 表示中の画面を PNG にしてシリアルへ送る（受信側: tools/screenshot.py）---
static void sendScreenshot() {
  size_t len = 0;
  void* png = M5.Display.createPng(&len, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  if (png == nullptr) {
    Serial.println("SCREENSHOT:ERROR");
    return;
  }
  Serial.flush();
  Serial.printf("\nSCREENSHOT:BEGIN %u\n", (unsigned)len);
  Serial.write((const uint8_t*)png, len);
  Serial.println("\nSCREENSHOT:END");
  Serial.flush();
  free(png);
}

//--- シリアルから時刻設定・スクリーンショット要求 ---
void handleSerialTimeSet() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        // "2025/06/14 08:30:00" 形式で解析
        int year, month, day, hour, minute, second;
        serialBuffer.trim();
        if (serialBuffer == "screenshot") {
          sendScreenshot();
        } else if (sscanf(serialBuffer.c_str(), "%d/%d/%d %d:%d:%d",
                   &year, &month, &day, &hour, &minute, &second) == 6) {

          if (rtc_ok) {
            struct tm t;
            t.tm_year = year - 1900;  // tm_yearは1900年からの年数
            t.tm_mon = month - 1;     // tm_monは0-11
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min = minute;
            t.tm_sec = second;

            if (rtc.setTime(t)) {
              Serial.println("RTC時刻を設定しました: " + formatDateTime(t));
            } else {
              Serial.println("エラー: RTC時刻設定に失敗しました");
            }
          } else {
            Serial.println("エラー: RTCが初期化されていません");
          }
        } else {
          Serial.println("エラー: 時刻形式が正しくありません");
          Serial.println("正しい形式: YYYY/MM/DD HH:MM:SS");
          Serial.println("例: 2025/06/14 08:30:00");
        }
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
}

//--- スケジュール読み込み ---
void loadSchedules() {
  if (!sdcard_ok) {
    Serial.println("SDカードが利用できません");
    return;
  }

  File file = SD_MMC.open(SCHEDULE_FILE);
  if (!file) {
    Serial.println("スケジュールファイルが見つかりません: " + String(SCHEDULE_FILE));
    return;
  }

  Serial.println("スケジュールファイルを読み込み中...");
  String csvContent;
  while (file.available()) {
    csvContent += (char)file.read();
  }
  file.close();

  // CSV_Parserのコンストラクタでデータを渡す
  CSV_Parser cp((char*)csvContent.c_str(), "sss", true, ',');

  char **start = (char **)cp[0];
  char **stop = (char **)cp[1];
  char **action = (char **)cp[2];
  size_t rows = cp.getRowsCount();

  Serial.println("読み込まれた行数: " + String(rows));

  schedules.clear();
  for (size_t i = 0; i < rows; ++i) {
    Schedule sch;
    sch.start = parseDateTime(String(start[i]));
    sch.stop  = parseDateTime(String(stop[i]));
    String s = String(action[i]);
    // BOM除去
    if (i == 0 && s.startsWith("\xEF\xBB\xBF")) s.remove(0, 3);
    sch.action = s;
    schedules.push_back(sch);
    Serial.println("スケジュール" + String(i+1) + ": " + sch.action);
  }
}

//--- NTP同期 ---
bool syncNTP() {
  // JST (UTC+9) をPOSIX形式で指定してNTP同期開始
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");

  // NTPが実際に同期完了するまで待機（最大10秒）
  for (int i = 0; i < 20; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      break;
    }
    delay(500);
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (rtc_ok) {
      rtc.setTime(timeinfo);
    }
    Serial.println("NTP同期成功: " + formatDateTime(timeinfo));
    return true;
  }
  return false;
}

//--- Wi-Fi初期化とNTP同期 ---
void initWiFi() {
  if (standalone_mode) {
    Serial.println("スタンドアロンモード: Wi-Fiスキップ");
    WiFi.mode(WIFI_OFF);
    bootStep(STEP_WIFI, BOOT_SKIP, "スタンドアロンモード");
    bootStep(STEP_NTP, BOOT_SKIP, "RTC時刻を使用");
    return;
  }

  // M5Tab5のWiFi (ESP32-C6) はSDIO2に接続されているためピン指定が必須
  // CLK=12, CMD=13, D0=11, D1=10, D2=9, D3=8, RST=15
  WiFi.setPins(12, 13, 11, 10, 9, 8, 15);

  // パニックリセット後はスキップ（フェイルセーフ）
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
    Serial.println("Wi-Fi: パニックリセット後 - スキップ");
    WiFi.mode(WIFI_OFF);
    bootStep(STEP_WIFI, BOOT_FAIL, "前回クラッシュのためスキップ");
    bootStep(STEP_NTP, BOOT_SKIP, "RTC時刻を使用");
    return;
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  bootHint = "未設定時はSSID「M5Tab5_Setup」に接続して設定";
  bootStep(STEP_WIFI, BOOT_RUNNING, "接続中…");

  if (wm.autoConnect("M5Tab5_Setup")) {
    wifi_ok = true;
    Serial.println("Wi-Fi接続成功: " + WiFi.localIP().toString());
    bootHint = "";
    bootStep(STEP_WIFI, BOOT_OK, WiFi.localIP().toString());
    bootStep(STEP_NTP, BOOT_RUNNING, "同期中…");

    if (syncNTP()) {
      bootStep(STEP_NTP, BOOT_OK, "RTC更新済み");
    } else {
      Serial.println("NTP同期失敗 - RTC時刻を使用");
      bootStep(STEP_NTP, BOOT_WARN, "失敗 - RTC時刻を使用");
    }
  } else {
    Serial.println("Wi-Fi接続失敗 - RTC時刻を使用");
    WiFi.mode(WIFI_OFF);
    bootHint = "";
    bootStep(STEP_WIFI, BOOT_FAIL, "接続失敗");
    bootStep(STEP_NTP, BOOT_SKIP, "RTC時刻を使用");
  }
}

//--- SDカード初期化 ---
void initSDCard() {
  Serial.println("SD_MMCでSDカード初期化開始...");

  if (!SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
    Serial.println("SD_MMC 1ビットモード失敗");

    // 4ビットモードで再試行
    if (!SD_MMC.begin("/sdcard", false)) {  // false = 4-bit mode
      Serial.println("SD_MMC 4ビットモード失敗");
      sdcard_ok = false;
      return;
    } else {
      Serial.println("SD_MMC 4ビットモード成功");
    }
  } else {
    Serial.println("SD_MMC 1ビットモード成功");
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SDカードが挿入されていません");
    sdcard_ok = false;
    return;
  }

  sdcard_ok = true;
  Serial.print("SDカードタイプ: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.println("SDカードサイズ: " + String(cardSize) + "MB");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // タイムゾーンをJSTに固定（NTP/WiFi未接続時も有効）
  setenv("TZ", "JST-9", 1);
  tzset();

  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(3);  // 横向き
  M5.Display.fillScreen(C_BG);

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);

  // 初期化画面
  showBootScreen();

  // RTC初期化
  bootStep(STEP_RTC, BOOT_RUNNING, "初期化中…");
  initRTC();

  if (rtc_ok) {
    struct tm t;
    if (isRTCTimeValid() && rtc.getTime(&t)) {
      bootStep(STEP_RTC, BOOT_OK, formatDateTime(t));
    } else {
      bootStep(STEP_RTC, BOOT_WARN, "時刻未設定");
    }
  } else {
    bootStep(STEP_RTC, BOOT_FAIL, "RX8130CEが見つかりません");
  }

  // 起動モード選択（5秒でスタンドアロン自動移行）
  showModeSelectionScreen();
  showBootScreen();

  // Wi-Fi + NTP（スタンドアロンモード時はスキップ）
  initWiFi();

  // スタンドアロンモードかつRTC時刻が未設定の場合は時刻設定を促す
  if (standalone_mode && !isRTCTimeValid()) {
    showTimeSetScreen();
    showBootScreen();
  }

  // SDカード初期化
  bootStep(STEP_SD, BOOT_RUNNING, "初期化中…");
  initSDCard();

  if (sdcard_ok) {
    bootStep(STEP_SD, BOOT_OK, "OK");
    bootStep(STEP_SCHEDULE, BOOT_RUNNING, "読み込み中…");
    loadSchedules();

    if (schedules.size() > 0) {
      bootStep(STEP_SCHEDULE, BOOT_OK, String(schedules.size()) + "件");
    } else {
      bootStep(STEP_SCHEDULE, BOOT_WARN, "見つかりません");
    }
  } else {
    bootStep(STEP_SD, BOOT_FAIL, "初期化失敗");
    bootStep(STEP_SCHEDULE, BOOT_SKIP, "");
  }

  // 使用方法の表示
  Serial.println("\n=== RTC時刻設定方法 ===");
  Serial.println("シリアルで以下の形式で時刻を送信してください:");
  Serial.println("YYYY/MM/DD HH:MM:SS");
  Serial.println("例: 2025/06/14 08:30:00");
  Serial.println("画面のスクリーンショット: screenshot（tools/screenshot.py で受信）");

  delay(2000); // 初期化結果を表示
}

//==================== メイン画面 ====================
//--- 右端 xRight に揃えてステータスチップを描き、左端の x を返す ---
static int drawChip(int xRight, const String& label, bool withDot, uint32_t dot) {
  const int y = 10, h = 36;
  canvas.setFont(F_JP24);
  canvas.setTextSize(1);
  int w = canvas.textWidth(label) + 32 + (withDot ? 20 : 0);
  int x = xRight - w;
  canvas.fillSmoothRoundRect(x, y, w, h, h / 2, C_BORDER);
  canvas.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, h / 2 - 1, C_CARD);
  int tx = x + 16;
  if (withDot) {
    canvas.fillSmoothCircle(tx + 6, y + h / 2, 6, dot);
    tx += 20;
  }
  drawText(label, tx, y + h / 2, F_JP24, C_TEXT, Datum::middle_left);
  return x;
}

static void renderHeader() {
  canvas.fillRect(R_HEADER.x, R_HEADER.y, R_HEADER.w, R_HEADER.h, C_BG);
  canvas.fillSmoothRoundRect(24, 14, 8, 28, 4, C_ACCENT);
  drawText("スケジュール", 44, 28, F_JP32, C_TEXT, Datum::middle_left);

  drawPillButton(BTN_TIME_SET, "時刻設定", C_ACCENT, C_WHITE, F_JP24, C_ACCENT);

  int x = BTN_TIME_SET.x - 16;
  x = drawChip(x, String(schedules.size()) + "件", false, 0) - 10;
  x = drawChip(x, "Wi-Fi", true, standalone_mode ? C_MUTED : (wifi_ok ? C_LIVE : C_ERR)) - 10;
  x = drawChip(x, "SD", true, sdcard_ok ? C_LIVE : C_ERR) - 10;
  drawChip(x, "RTC", true, rtc_ok ? C_LIVE : C_ERR);
}

//--- 秒表示の矩形（HH:MM の右隣、ベースライン揃え）---
static Rect secondsRect() {
  canvas.setFont(F_NUM);
  canvas.setTextSize(CLOCK_SIZE);
  int x = CLOCK_X + canvas.textWidth("00:00") + 12;
  int capH = (int)(52 * SEC_SIZE);
  return { x, CLOCK_BASE - capH - 10, 200, capH + 20 };
}

static void drawSeconds(const struct tm& t, bool valid) {
  Rect r = secondsRect();
  canvas.fillRect(r.x, r.y, r.w, r.h, C_CARD);
  char buf[8];
  if (valid) snprintf(buf, sizeof(buf), ":%02d", t.tm_sec);
  else       strcpy(buf, ":--");
  drawText(buf, r.x, CLOCK_BASE, F_NUM, valid ? C_SUB : C_ERR, Datum::baseline_left, SEC_SIZE);
}

static void renderClock(const struct tm& t, bool valid) {
  clearCardArea(R_CLOCK);
  drawCard(R_CLOCK);

  char buf[8];
  if (valid) snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  else       strcpy(buf, "--:--");
  drawText(buf, CLOCK_X, CLOCK_BASE, F_NUM, valid ? C_TEXT : C_ERR, Datum::baseline_left, CLOCK_SIZE);
  drawSeconds(t, valid);

  int rx = R_CLOCK.x + R_CLOCK.w - 48;
  if (valid) {
    drawText(String(t.tm_year + 1900) + "年", rx, R_CLOCK.y + 70, F_JP28, C_SUB, Datum::top_right);
    drawText(String(t.tm_mon + 1) + "月" + String(t.tm_mday) + "日", rx, R_CLOCK.y + 108,
             F_JP40, C_TEXT, Datum::top_right, 1.25f);
    uint32_t wc = (t.tm_wday == 0) ? C_ERR : (t.tm_wday == 6) ? C_ACCENT : C_SUB;
    drawText(String(WDAY_JA[t.tm_wday]) + "曜日", rx, R_CLOCK.y + 172, F_JP32, wc, Datum::top_right);
  } else {
    drawText("時刻が未設定です", rx, R_CLOCK.y + 110, F_JP32, C_ERR, Datum::top_right);
    drawText("「時刻設定」で設定", rx, R_CLOCK.y + 156, F_JP24, C_SUB, Datum::top_right);
  }
}

static void drawMessageCard(uint32_t color, const char* title, const char* line1, const char* line2) {
  drawCard(R_BOTTOM);
  int cx = R_BOTTOM.x + 110, cy = R_BOTTOM.y + R_BOTTOM.h / 2;
  drawAlertIcon(cx, cy, 48, color);
  int tx = R_BOTTOM.x + 200;
  drawText(title, tx, cy - 50, F_JP40, color, Datum::middle_left);
  drawText(line1, tx, cy + 10, F_JP28, C_SUB, Datum::middle_left);
  drawText(line2, tx, cy + 54, F_JP28, C_SUB, Datum::middle_left);
}

static void renderOngoing(const std::vector<const Schedule*>& list, time_t now) {
  const Rect& r = R_ONGOING;
  drawCard(r);
  const int left = r.x + 28, right = r.x + r.w - 28;
  canvas.fillSmoothCircle(left + 8, r.y + 40, 8, C_LIVE);
  drawText("進行中", left + 26, r.y + 40, F_JP32, C_TEXT, Datum::middle_left);
  if (!list.empty()) {
    drawText(String(list.size()) + "件", right, r.y + 40, F_JP28, C_SUB, Datum::middle_right);
  }

  const int top = r.y + 76, bottom = r.y + r.h - 16;
  if (list.empty()) {
    drawText("現在進行中の予定はありません", r.x + r.w / 2, (top + bottom) / 2, F_JP32, C_SUB, Datum::middle_center);
    return;
  }

  // 件数が少ないほど大きく表示
  int n = std::min<int>(list.size(), ONGOING_VISIBLE);
  int rowH = (bottom - top) / n;
  const lgfx::IFont* metaFont = (n >= 3) ? F_JP24 : F_JP28;
  int   metaH     = (n >= 3) ? 24 : 28;
  float titleSize = (n == 1) ? 1.5f : 1.0f;
  int   titleH    = (int)(40 * titleSize);
  int   contentH  = metaH + 6 + titleH + 8 + 6;
  const int cx = left + 24, cw = right - cx;

  for (int i = 0; i < n; i++) {
    const Schedule& s = *list[i];
    int y = top + i * rowH + (rowH - contentH) / 2;
    canvas.fillSmoothRoundRect(left, y, 6, contentH, 3, C_LIVE);
    drawText(formatRange(s), cx, y, metaFont, C_SUB, Datum::top_left);
    drawText("残り " + formatDuration(s.stop - now), right, y, metaFont, C_LIVE, Datum::top_right);
    drawText(fitText(s.action, cw, F_JP40, titleSize), cx, y + metaH + 6, F_JP40, C_TEXT, Datum::top_left, titleSize);
    float p = (float)(now - s.start) / (float)std::max<time_t>(1, s.stop - s.start);
    drawProgress(cx, y + contentH - 6, cw, 6, p, C_LIVE);
  }
}

static void renderFuture(const std::vector<const Schedule*>& list, time_t now) {
  const Rect& r = R_FUTURE;
  drawCard(r);
  const int left = r.x + 24, right = r.x + r.w - 24;
  canvas.fillSmoothCircle(left + 8, r.y + 40, 8, C_ACCENT);
  drawText("次の予定", left + 26, r.y + 40, F_JP32, C_TEXT, Datum::middle_left);

  int total = list.size();
  future_max_offset    = std::max(0, total - FUTURE_VISIBLE);
  future_scroll_offset = constrain(future_scroll_offset, 0, future_max_offset);
  drawArrowButton(BTN_UP,   true,  future_scroll_offset > 0);
  drawArrowButton(BTN_DOWN, false, future_scroll_offset < future_max_offset);

  const int top = r.y + 76, bottom = r.y + r.h - 48;
  if (total == 0) {
    drawText("今後の予定はありません", (left + right) / 2, (top + bottom) / 2, F_JP28, C_SUB, Datum::middle_center);
    return;
  }

  int shown = std::min(FUTURE_VISIBLE, total - future_scroll_offset);
  int rowH  = (bottom - top) / FUTURE_VISIBLE;
  const int cw = right - left;
  for (int i = 0; i < shown; i++) {
    const Schedule& s = *list[future_scroll_offset + i];
    int y = top + i * rowH + (rowH - 100) / 2;
    if (i > 0) canvas.drawFastHLine(left, top + i * rowH, cw, C_BORDER);

    String when = isSameDay(s.start, now) ? formatRange(s) : formatMonthDay(s.start) + " " + formatRange(s);
    drawText(fitText(when, cw, F_JP28), left, y, F_JP28, C_ACCENT, Datum::top_left);

    // 長いタイトルは一段小さいフォントにしてから省略
    canvas.setFont(F_JP40);
    canvas.setTextSize(1);
    const lgfx::IFont* tf = (canvas.textWidth(s.action) > cw) ? F_JP32 : F_JP40;
    drawText(fitText(s.action, cw, tf), left, (tf == F_JP40) ? y + 32 : y + 36, tf, C_TEXT, Datum::top_left);

    drawText("あと " + formatDuration(s.start - now), left, y + 76, F_JP24, C_SUB, Datum::top_left);
  }

  String pos = String(future_scroll_offset + 1);
  if (shown > 1) pos += "-" + String(future_scroll_offset + shown);
  pos += " / " + String(total);
  drawText(pos, right, r.y + r.h - 16, F_JP24, C_SUB, Datum::bottom_right);
}

static void renderBottom(time_t now, bool valid) {
  clearCardArea(R_BOTTOM);
  future_max_offset = 0;

  if (!rtc_ok) {
    drawMessageCard(C_ERR, "RTCに接続できません",
                    "右上の「時刻設定」ボタン、またはシリアルで設定できます",
                    "シリアル入力例: 2025/06/14 08:30:00");
    return;
  }
  if (!valid) {
    drawMessageCard(C_ERR, "RTCから時刻を読み取れません", "RTCの接続を確認してください", "");
    return;
  }
  if (!sdcard_ok) {
    drawMessageCard(C_ERR, "SDカードを確認してください", "microSDカードが挿入されているか確認してください", "");
    return;
  }
  if (schedules.empty()) {
    drawMessageCard(C_WARN, "スケジュールが見つかりません",
                    "・SDカード直下に schedule.csv があるか",
                    "・CSV形式（start,stop,action）が正しいか");
    return;
  }

  std::vector<const Schedule*> ongoing, future;
  for (auto& s : schedules) {
    if (now >= s.start && now < s.stop) ongoing.push_back(&s);
    else if (s.start > now)             future.push_back(&s);
  }
  auto byStart = [](const Schedule* a, const Schedule* b) { return a->start < b->start; };
  std::stable_sort(ongoing.begin(), ongoing.end(), byStart);
  std::stable_sort(future.begin(), future.end(), byStart);

  renderOngoing(ongoing, now);
  renderFuture(future, now);
}

//--- メイン画面の状態 ---
bool     needFullRedraw   = true;
bool     needBottomRedraw = false;
bool     prevTimeValid    = false;
time_t   prevShownTime    = 0;
uint32_t lastTimePoll     = 0;
const uint32_t TIME_POLL_MS = 100;

static void handleMainTouch() {
  M5.update();
  auto td = M5.Touch.getDetail();
  if (!td.wasPressed()) return;

  if (BTN_TIME_SET.contains(td.x, td.y, 12)) {
    showTimeSetScreen();
    needFullRedraw = true;
  } else if (BTN_UP.contains(td.x, td.y, 6)) {
    if (future_scroll_offset > 0) {
      future_scroll_offset--;
      needBottomRedraw = true;
    }
  } else if (BTN_DOWN.contains(td.x, td.y, 6)) {
    if (future_scroll_offset < future_max_offset) {
      future_scroll_offset++;
      needBottomRedraw = true;
    }
  }
}

void loop() {
  // シリアルからの時刻設定をチェック
  handleSerialTimeSet();
  handleMainTouch();

  if (!needFullRedraw && !needBottomRedraw && millis() - lastTimePoll < TIME_POLL_MS) {
    delay(10);
    return;
  }
  lastTimePoll = millis();

  // 現在時刻取得
  struct tm t = {};
  bool time_valid = rtc_ok && rtc.getTime(&t);
  time_t current_time = 0;
  if (time_valid) {
    current_time = mktime(&t);  // 曜日(tm_wday)もここで確定する
  }

  if (needFullRedraw) {
    canvas.fillScreen(C_BG);
    renderHeader();
    renderClock(t, time_valid);
    renderBottom(current_time, time_valid);
    pushArea(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  } else {
    // 予定の開始・終了は分単位なので、分が変わったときに予定欄もまとめて更新する
    bool minuteChanged = (time_valid != prevTimeValid) || (current_time / 60 != prevShownTime / 60);
    if (minuteChanged) {
      renderClock(t, time_valid);
      renderBottom(current_time, time_valid);
      pushCardArea(R_CLOCK);
      pushCardArea(R_BOTTOM);
    } else {
      if (current_time != prevShownTime) {
        // 毎秒の更新は秒表示の小領域だけを転送
        drawSeconds(t, time_valid);
        Rect s = secondsRect();
        pushArea(s.x, s.y, s.w, s.h);
      }
      if (needBottomRedraw) {
        renderBottom(current_time, time_valid);
        pushCardArea(R_BOTTOM);
      }
    }
  }

  needFullRedraw   = false;
  needBottomRedraw = false;
  prevTimeValid    = time_valid;
  prevShownTime    = current_time;
  delay(10);
}

/*
=== 使用方法 ===
1. 必要なライブラリをインストール:
   - ArtronShop_RX8130CE by ArtronShop

2. SDカードに schedule.csv を配置
   CSVファイル例:
   start,stop,action
   2025/6/8 9:00,2025/6/8 10:00,会議
   2025/6/8 14:00,2025/6/8 15:00,プレゼン

3. コンパイル・アップロード・実行

4. シリアルモニターを開いて時刻設定:
   例: 2025/06/14 08:30:00

RX8130CEの初期化に失敗する場合は、ハードウェア接続を確認してください。
*/
