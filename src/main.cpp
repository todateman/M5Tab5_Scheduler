#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <SD_MMC.h>
#include <CSV_Parser.h>
#include <Wire.h>
#include <ArtronShop_RX8130CE.h>
#include <esp_sntp.h>

//==================== 定数・マクロ ====================
const char* SCHEDULE_FILE = "/schedule.csv";

// Tab5 RTC設定
ArtronShop_RX8130CE rtc(&Wire1);
bool rtc_ok = false;
bool sdcard_ok = false;
bool wifi_ok = false;
bool standalone_mode = false;
int future_scroll_offset = 0;
LGFX_Button btn_time_set, btn_scroll_up, btn_scroll_down;

const int SCREEN_WIDTH = 1280;
const int SCREEN_HEIGHT = 720;

M5Canvas canvas(&M5.Display);

const int TIME_TEXT_X = 50;
const int TIME_TEXT_Y = 70;

bool   firstDraw             = true;
bool   prevTimeValid         = false;
time_t prevFirstOngoingStart = 0;
time_t prevFirstFutureStart  = 0;

struct Schedule {
  time_t start;
  time_t stop;
  String action;
};
std::vector<Schedule> schedules;

//--- シリアル時刻設定用のバッファ ---
String serialBuffer = "";

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

//--- 現在時刻をtime_tで取得 ---
time_t getCurrentUnixTime() {
  if (!rtc_ok) return 0;
  
  struct tm t;
  if (!rtc.getTime(&t)) {
    return 0;
  }
  
  return mktime(&t);
}

//--- RTC時刻が有効（2020年以降）かチェック ---
bool isRTCTimeValid() {
  if (!rtc_ok) return false;
  struct tm t;
  if (!rtc.getTime(&t)) return false;
  return (t.tm_year + 1900) >= 2020;
}

//--- 起動モード選択画面（5秒でスタンドアロンへ自動移行）---
void showModeSelectionScreen() {
  M5.Display.fillScreen(TFT_YELLOW);
  M5.Display.setFont(&fonts::lgfxJapanGothic_40);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setCursor(50, 30);
  M5.Display.println("Tab5 スケジュール表示 - 起動モード選択");

  // ボタン定義（cx/cy はセンター座標）
  LGFX_Button btn_wifi, btn_sa;
  btn_wifi.initButton(&M5.Display, 350, 350, 400, 200, TFT_DARKCYAN, TFT_CYAN,      TFT_BLACK, "", 1.0f);
  btn_sa.initButton(  &M5.Display, 930, 350, 400, 200, TFT_DARKGRAY, TFT_LIGHTGRAY, TFT_BLACK, "", 1.0f);
  btn_wifi.drawButton();
  btn_sa.drawButton();

  // 日本語ラベル（LGFX_Buttonのlabelはボタン描画後に上書き）
  M5.Display.setTextSize(1);   M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setCursor(220, 315); M5.Display.println("Wi-Fi接続");
  M5.Display.setTextSize(0.7);
  M5.Display.setCursor(195, 375); M5.Display.println("(NTP時刻同期)");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(760, 315); M5.Display.println("スタンドアロン");
  M5.Display.setTextSize(0.7);
  M5.Display.setCursor(775, 375); M5.Display.println("(RTC時刻使用)");

  for (int countdown = 5; countdown > 0; countdown--) {
    M5.Display.fillRect(0, 500, SCREEN_WIDTH, 80, TFT_YELLOW);
    M5.Display.setTextSize(0.8);
    M5.Display.setTextColor(TFT_DARKGRAY);
    M5.Display.setCursor(300, 510);
    M5.Display.println(String(countdown) + " 秒後に自動でスタンドアロンモードで起動します...");

    for (int i = 0; i < 20; i++) {  // 50ms × 20 = 1秒
      M5.update();
      auto td = M5.Touch.getDetail();
      bool p = td.isPressed();
      btn_wifi.press(p && btn_wifi.contains(td.x, td.y));
      btn_sa.press(p   && btn_sa.contains(td.x, td.y));

      if (btn_wifi.justPressed()) {
        standalone_mode = false;
        btn_wifi.drawButton(true);
        delay(400);
        return;
      }
      if (btn_sa.justPressed()) {
        standalone_mode = true;
        btn_sa.drawButton(true);
        delay(400);
        return;
      }
      delay(50);
    }
  }
  standalone_mode = true;  // タイムアウト → スタンドアロン
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

  // ボタン初期化（cx/cy はセンター座標）
  LGFX_Button btns_m[6], btns_p[6], btn_ok;
  for (int i = 0; i < 6; i++) {
    int cy = 120 + i * 90 + 35;
    btns_m[i].initButton(&M5.Display, 595, cy, 90, 70, TFT_WHITE, TFT_NAVY,      TFT_WHITE, "-", 1.5f);
    btns_p[i].initButton(&M5.Display, 725, cy, 90, 70, TFT_WHITE, TFT_NAVY,      TFT_WHITE, "+", 1.5f);
  }
  btn_ok.initButton(&M5.Display, 640, 678, 400, 60, TFT_WHITE, TFT_DARKGREEN, TFT_WHITE, "", 1.0f);

  auto drawUI = [&]() {
    M5.Display.fillScreen(0x2104);
    M5.Display.setFont(&fonts::lgfxJapanGothic_40);
    M5.Display.setTextSize(1.2);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(500, 30);
    M5.Display.println("時刻設定");

    for (int i = 0; i < 6; i++) {
      int rowY = 120 + i * 90;
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(TFT_LIGHTGRAY);
      M5.Display.setCursor(200, rowY + 10);
      M5.Display.print(labels[i]); M5.Display.print(":");

      int dispVal = (i == 0) ? *fields[i] + 1900 : (i == 1) ? *fields[i] + 1 : *fields[i];
      char valBuf[8];
      sprintf(valBuf, (i == 0) ? "%4d" : "%2d", dispVal);
      M5.Display.setTextSize(1.2);
      M5.Display.setTextColor(TFT_WHITE);
      M5.Display.setCursor(350, rowY + 5);
      M5.Display.print(valBuf);

      btns_m[i].drawButton();
      btns_p[i].drawButton();
    }
    btn_ok.drawButton();
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(490, 665);
    M5.Display.println("確定・RTC保存");
  };

  drawUI();

  while (true) {
    M5.update();
    auto td  = M5.Touch.getDetail();
    bool p   = td.isPressed();

    for (int i = 0; i < 6; i++) {
      btns_m[i].press(p && btns_m[i].contains(td.x, td.y));
      btns_p[i].press(p && btns_p[i].contains(td.x, td.y));
    }
    btn_ok.press(p && btn_ok.contains(td.x, td.y));

    bool changed = false;
    for (int i = 0; i < 6; i++) {
      if (btns_m[i].justPressed() && *fields[i] > mins[i]) { (*fields[i])--; changed = true; }
      if (btns_p[i].justPressed() && *fields[i] < maxs[i]) { (*fields[i])++; changed = true; }
    }
    if (changed) { drawUI(); delay(150); continue; }

    if (btn_ok.justPressed()) {
      t.tm_isdst = -1;
      if (rtc_ok) {
        rtc.setTime(t);
        Serial.println("RTC時刻を設定しました: " + formatDateTime(t));
      }
      time_t ts = mktime(&t);
      struct timeval tv = { ts, 0 };
      settimeofday(&tv, nullptr);
      break;
    }
    delay(50);
  }
}

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

//--- シリアルから時刻設定 ---
void handleSerialTimeSet() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        // "2025/06/14 08:30:00" 形式で解析
        int year, month, day, hour, minute, second;
        if (sscanf(serialBuffer.c_str(), "%d/%d/%d %d:%d:%d", 
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
    M5.Display.setCursor(50, 150);
    M5.Display.setTextColor(TFT_ORANGE);
    M5.Display.println("Wi-Fi: 前回クラッシュ - スキップ");
    return;
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  M5.Display.setCursor(50, 150);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.println("Wi-Fi接続中... (SSID: M5Tab5_Setup)");

  if (wm.autoConnect("M5Tab5_Setup")) {
    wifi_ok = true;
    Serial.println("Wi-Fi接続成功: " + WiFi.localIP().toString());

    M5.Display.setCursor(50, 200);
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.println("Wi-Fi接続成功: " + WiFi.localIP().toString());

    M5.Display.setCursor(50, 250);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.println("NTP時刻同期中...");

    if (syncNTP()) {
      M5.Display.setCursor(50, 300);
      M5.Display.setTextColor(TFT_DARKGREEN);
      M5.Display.println("NTP同期成功 - RTC更新済み");
    } else {
      Serial.println("NTP同期失敗 - RTC時刻を使用");
      M5.Display.setCursor(50, 300);
      M5.Display.setTextColor(TFT_ORANGE);
      M5.Display.println("NTP同期失敗 - RTC時刻を使用");
    }
  } else {
    Serial.println("Wi-Fi接続失敗 - RTC時刻を使用");
    WiFi.mode(WIFI_OFF);
    M5.Display.setCursor(50, 200);
    M5.Display.setTextColor(TFT_ORANGE);
    M5.Display.println("Wi-Fi接続失敗 - RTC時刻を使用");
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
  
  // 背景色設定
  M5.Display.setRotation(3);  // 横向き
  M5.Display.fillScreen(TFT_YELLOW);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setFont(&fonts::lgfxJapanGothic_40);
  M5.Display.setTextSize(1);

  canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
  canvas.setFont(&fonts::lgfxJapanGothic_40);
  canvas.setTextSize(1);

  // メイン画面タッチボタン初期化（cx/cy はセンター座標）
  btn_time_set.initButton(  &canvas, 130,  685,  220, 55, TFT_DARKGRAY, TFT_LIGHTGRAY, TFT_BLACK, "", 1.0f);
  btn_scroll_up.initButton( &canvas, 1235, 383,   60, 60, TFT_DARKGRAY, TFT_DARKGRAY,  TFT_WHITE, "^", 1.0f);
  btn_scroll_down.initButton(&canvas, 1235, 623,  60, 60, TFT_DARKGRAY, TFT_DARKGRAY,  TFT_WHITE, "v", 1.0f);


  // 初期化画面
  M5.Display.setCursor(50, 50);
  M5.Display.println("Tab5 スケジュール表示");
  M5.Display.setCursor(50, 100);
  M5.Display.println("初期化中...");

  // RTC初期化
  initRTC();

  if (rtc_ok) {
    Serial.println("RTC初期化成功");
  }

  // 起動モード選択（5秒でスタンドアロン自動移行）
  showModeSelectionScreen();

  // Wi-Fi + NTP（スタンドアロンモード時はスキップ）
  initWiFi();

  // スタンドアロンモードかつRTC時刻が未設定の場合は時刻設定を促す
  if (standalone_mode && !isRTCTimeValid()) {
    showTimeSetScreen();
  }

  // SDカード初期化
  M5.Display.setCursor(50, 350);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.println("SDカード初期化中...");
  initSDCard();

  if (sdcard_ok) {
    M5.Display.setCursor(50, 400);
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.println("SDカード初期化成功");

    M5.Display.setCursor(50, 450);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.println("スケジュール読み込み中...");
    loadSchedules();

    if (schedules.size() > 0) {
      M5.Display.setCursor(50, 500);
      M5.Display.setTextColor(TFT_DARKGREEN);
      M5.Display.println("スケジュール読み込み成功: " + String(schedules.size()) + "件");
    } else {
      M5.Display.setCursor(50, 500);
      M5.Display.setTextColor(TFT_ORANGE);
      M5.Display.println("スケジュールが見つかりません");
    }
  } else {
    M5.Display.setCursor(50, 400);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.println("SDカード初期化失敗");
  }

  // 使用方法の表示
  Serial.println("\n=== RTC時刻設定方法 ===");
  Serial.println("シリアルで以下の形式で時刻を送信してください:");
  Serial.println("YYYY/MM/DD HH:MM:SS");
  Serial.println("例: 2025/06/14 08:30:00");
  
  delay(3000); // 初期化メッセージを表示
}

//--- 現在時刻を M5.Display に直接描画（背景色付きで前フレームを上書き）---
static void drawTimeDirect(const String& datetime, bool time_valid) {
  M5.Display.setFont(&fonts::lgfxJapanGothic_40);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(TIME_TEXT_X, TIME_TEXT_Y);
  M5.Display.setTextColor(time_valid ? TFT_BLACK : TFT_RED, TFT_YELLOW);
  M5.Display.println("現在時刻:\n " + datetime);
}

void loop() {
  // シリアルからの時刻設定をチェック
  handleSerialTimeSet();

  // 現在時刻取得
  String datetime;
  bool time_valid = false;
  time_t current_time = 0;

  if (rtc_ok) {
    struct tm t;
    if (rtc.getTime(&t)) {
      datetime = formatDateTime(t);
      current_time = getCurrentUnixTime();
      time_valid = true;
    } else {
      datetime = "RTC読み取りエラー";
    }
  } else {
    datetime = "RTC未接続";
  }

  // スケジュール変化を検出（最初の進行中・未来スケジュールの start で比較）
  time_t firstOngoingStart = 0;
  time_t firstFutureStart  = 0;
  for (auto& s : schedules) {
    if (time_valid && current_time >= s.start && current_time < s.stop) {
      if (firstOngoingStart == 0) firstOngoingStart = s.start;
    }
    if (time_valid && s.start > current_time) {
      if (firstFutureStart == 0) firstFutureStart = s.start;
    }
  }

  bool doFullDraw = firstDraw
      || (time_valid != prevTimeValid)
      || (firstOngoingStart != prevFirstOngoingStart)
      || (firstFutureStart  != prevFirstFutureStart);

  if (doFullDraw) {
    //=== 全画面再描画（スケジュール変化時のみ）===
    canvas.fillScreen(TFT_YELLOW);

    //--- タイトルと現在時刻（上部） ---
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_BLACK);
    canvas.setCursor(50, 20);
    canvas.println("Tab5 スケジュール表示");

    canvas.setTextSize(3);
    canvas.setCursor(TIME_TEXT_X, TIME_TEXT_Y);
    canvas.setTextColor(time_valid ? TFT_BLACK : TFT_RED);
    canvas.println("現在時刻:\n " + datetime);

    //--- ステータス表示 ---
    canvas.setTextSize(0.8);
    canvas.setCursor(900, 20);
    if (rtc_ok) {
      canvas.setTextColor(TFT_DARKGREEN);
      canvas.print("RTC:OK  ");
    } else {
      canvas.setTextColor(TFT_RED);
      canvas.print("RTC:NG  ");
    }

    canvas.setCursor(900, 55);
    if (sdcard_ok) {
      canvas.setTextColor(TFT_DARKGREEN);
      canvas.print("SDカード:OK  ");
    } else {
      canvas.setTextColor(TFT_RED);
      canvas.print("SDカード:NG  ");
    }

    canvas.setCursor(900, 90);
    if (wifi_ok) {
      canvas.setTextColor(TFT_DARKGREEN);
      canvas.print("Wi-Fi:OK  ");
    } else {
      canvas.setTextColor(TFT_ORANGE);
      canvas.print("Wi-Fi:NG  ");
    }

    canvas.setCursor(900, 125);
    if (schedules.size() > 0) {
      canvas.setTextColor(TFT_DARKGREEN);
      canvas.print("スケジュール:" + String(schedules.size()) + "件");
    }

    if (sdcard_ok && schedules.size() > 0 && time_valid) {
      //--- 進行中の予定 ---
      int ongoing = 0;
      String ongoingTimes[3];
      String ongoingActions[3];
      for (auto& s : schedules) {
        if (current_time >= s.start && current_time < s.stop) {
          if (ongoing < 3) {
            // localtime()は静的メモリを使うため、結果をコピーして使用
            struct tm* temp1 = localtime(&s.start);
            struct tm start_tm = *temp1;

            struct tm* temp2 = localtime(&s.stop);
            struct tm stop_tm = *temp2;

            char buf[32];
            sprintf(buf, "%02d:%02d～%02d:%02d",
                    start_tm.tm_hour, start_tm.tm_min,
                    stop_tm.tm_hour, stop_tm.tm_min);
            ongoingTimes[ongoing] = String(buf);
            ongoingActions[ongoing] = s.action;
            ongoing++;
          }
        }
      }

      canvas.setTextSize(1.2);
      canvas.setTextColor(TFT_RED);
      canvas.setCursor(50, 310);
      canvas.println("【進行中の予定】");

      if (ongoing == 0) {
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_BLACK);
        canvas.setCursor(50, 360);
        canvas.println("現在進行中の予定はありません");
      } else {
        for (int i = 0; i < ongoing; ++i) {
          canvas.setTextSize(0.9);
          canvas.setTextColor(TFT_PURPLE);
          canvas.setCursor(70, 360 + i * 70);
          canvas.println(ongoingTimes[i]);

          canvas.setTextSize(1);
          canvas.setTextColor(TFT_BLACK);
          canvas.setCursor(70, 360 + i * 70 + 30);
          canvas.println("・" + ongoingActions[i]);
        }
      }

      //--- 未来の予定 ---
      int future = 0;
      String futureTimes[8];
      String futureActions[8];
      for (auto& s : schedules) {
        if (s.start > current_time && future < 8) {
          // localtime()は静的メモリを使うため、結果をコピーして使用
          struct tm* temp1 = localtime(&s.start);
          struct tm start_tm = *temp1;

          struct tm* temp2 = localtime(&s.stop);
          struct tm stop_tm = *temp2;

          char buf[32];
          sprintf(buf, "%02d:%02d～%02d:%02d",
                  start_tm.tm_hour, start_tm.tm_min,
                  stop_tm.tm_hour, stop_tm.tm_min);
          futureTimes[future] = String(buf);
          futureActions[future++] = s.action;
        }
      }

      canvas.setTextSize(1.2);
      canvas.setTextColor(TFT_BLUE);
      canvas.setCursor(690, 310);
      canvas.println("【今後の予定】");

      // スクロール範囲クランプ
      int maxOffset = (future > 4) ? (future - 4) : 0;
      if (future_scroll_offset > maxOffset) future_scroll_offset = maxOffset;
      if (future_scroll_offset < 0) future_scroll_offset = 0;

      for (int i = 0; i < 4 && (future_scroll_offset + i) < future; ++i) {
        int idx = future_scroll_offset + i;
        canvas.setTextSize(0.9);
        canvas.setTextColor(TFT_PURPLE);
        canvas.setCursor(710, 360 + i * 70);
        canvas.println(futureTimes[idx]);

        canvas.setTextSize(1);
        canvas.setTextColor(TFT_BLACK);
        canvas.setCursor(710, 360 + i * 70 + 30);
        canvas.println("・" + futureActions[idx]);
      }

      // ▲▼スクロールボタン（右端）
      if (future_scroll_offset > 0) {
        btn_scroll_up.drawButton();
      }
      if (future_scroll_offset < maxOffset) {
        btn_scroll_down.drawButton();
        canvas.setTextSize(0.7);
        canvas.setTextColor(TFT_DARKGRAY);
        canvas.setCursor(690, 665);
        canvas.println("他 " + String(future - future_scroll_offset - 4) + " 件");
      }

      // 時刻設定ボタン（左下）
      btn_time_set.drawButton();
      canvas.setTextColor(TFT_BLACK);
      canvas.setTextSize(0.8);
      canvas.setCursor(40, 672);
      canvas.println("時刻設定");


    } else {
      // エラーメッセージ表示
      canvas.setTextSize(2);
      canvas.setCursor(50, 310);
      if (!rtc_ok) {
        canvas.setTextColor(TFT_RED);
        canvas.println("RTCエラー");
        canvas.setCursor(50, 360);
        canvas.setTextSize(1);
        canvas.println("左下「時刻設定」ボタンで設定できます");
        canvas.setCursor(50, 400);
        canvas.println("またはシリアル: 2025/06/14 08:30:00");
      } else if (!sdcard_ok) {
        canvas.setTextColor(TFT_RED);
        canvas.println("SDカードを確認してください");
      } else if (schedules.size() == 0) {
        canvas.setTextColor(TFT_ORANGE);
        canvas.println("スケジュールファイルを確認");
        canvas.setCursor(50, 360);
        canvas.setTextSize(1);
        canvas.println("・schedule.csvが存在するか");
        canvas.setCursor(50, 400);
        canvas.println("・CSV形式が正しいか");
      }
    }

    canvas.pushSprite(0, 0);

    firstDraw             = false;
    prevTimeValid         = time_valid;
    prevFirstOngoingStart = firstOngoingStart;
    prevFirstFutureStart  = firstFutureStart;

  } else {
    // 時刻テキストのみ M5.Display に直接描画（背景色付きで前フレームを上書き）
    drawTimeDirect(datetime, time_valid);
  }

  // 1秒間タッチ入力をポーリング（100ms × 10回）
  for (int i = 0; i < 10; i++) {
    M5.update();
    auto td = M5.Touch.getDetail();
    bool p  = td.isPressed();

    btn_time_set.press(  p && btn_time_set.contains(td.x, td.y));
    btn_scroll_up.press( p && btn_scroll_up.contains(td.x, td.y));
    btn_scroll_down.press(p && btn_scroll_down.contains(td.x, td.y));

    if (btn_time_set.justPressed()) {
      showTimeSetScreen();
      firstDraw = true;
      break;
    }
    if (btn_scroll_up.justPressed() && future_scroll_offset > 0) {
      future_scroll_offset--;
      firstDraw = true;
      break;
    }
    if (btn_scroll_down.justPressed()) {
      future_scroll_offset++;
      firstDraw = true;
      break;
    }
    delay(100);
  }
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