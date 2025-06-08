#include <Arduino.h>
#include <M5Unified.h>
#include <SD_MMC.h>
#include <CSV_Parser.h>

// Tab5でSD_MMCモードを試すバージョン
// Wi-Fiは完全に無効化してSDIOの競合を避ける

const char* SCHEDULE_FILE = "/schedule.csv";
bool sdcard_ok = false;

struct Schedule {
  time_t start;
  time_t stop;
  String action;
};
std::vector<Schedule> schedules;

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

void testSDMMC() {
  Serial.println("=== SD_MMC モードでテスト ===");
  
  // 1ビットモードで試行
  if (!SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
    Serial.println("❌ SD_MMC 1ビットモード失敗");
    
    // 4ビットモードで試行
    if (!SD_MMC.begin("/sdcard", false)) {  // false = 4-bit mode
      Serial.println("❌ SD_MMC 4ビットモード失敗");
      return;
    } else {
      Serial.println("✅ SD_MMC 4ビットモード成功");
    }
  } else {
    Serial.println("✅ SD_MMC 1ビットモード成功");
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("❌ SDカードが検出されません");
    return;
  }
  
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
  
  // ファイルリスト表示
  File root = SD_MMC.open("/");
  if (root) {
    Serial.println("=== ルートディレクトリの内容 ===");
    File file = root.openNextFile();
    while (file) {
      if (file.isDirectory()) {
        Serial.println("DIR: " + String(file.name()));
      } else {
        Serial.println("FILE: " + String(file.name()) + " (" + String(file.size()) + " bytes)");
      }
      file = root.openNextFile();
    }
    root.close();
  }
  
  // テストファイル作成
  File testFile = SD_MMC.open("/tab5_test.txt", FILE_WRITE);
  if (testFile) {
    testFile.println("Tab5 SD_MMC Test Success!");
    testFile.close();
    Serial.println("✅ ファイル書き込みテスト成功");
  } else {
    Serial.println("⚠️  ファイル書き込みテスト失敗");
  }
  
  sdcard_ok = true;
}

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
  
  // CSV_Parserでパース
  CSV_Parser cp((char*)csvContent.c_str(), "sss", true, ',');
  
  char **start = (char **)cp[0];
  char **stop = (char **)cp[1];
  char **action = (char **)cp[2];
  size_t rows = cp.getRowsCount();
  
  Serial.println("読み込まれた行数: " + String(rows));
  
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== M5Stack Tab5 SD_MMC テスト ===");
  
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.fillScreen(0x001F);  // 青背景
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setFont(&fonts::lgfxJapanGothic_28);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.println("Tab5 SD_MMC テスト");

  // Wi-Fiを完全に無効化（重要！）
  // WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi無効化完了");
  
  // ESP32-C6（Wi-Fiモジュール）も無効化を試行
  // これによりSDIOバスがSDカード専用になる
  
  M5.Display.setCursor(0, 40);
  M5.Display.println("SD_MMCモードでテスト中...");
  
  delay(2000);
  
  // SD_MMCテスト実行
  testSDMMC();
  
  if (sdcard_ok) {
    M5.Display.fillScreen(0x07E0);  // 緑色
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("SD_MMC接続成功！");
    
    // スケジュール読み込みテスト
    loadSchedules();
    
    if (schedules.size() > 0) {
      M5.Display.setCursor(0, 40);
      M5.Display.println("スケジュール読み込み成功");
      M5.Display.setCursor(0, 70);
      M5.Display.println("項目数: " + String(schedules.size()));
    }
    
  } else {
    M5.Display.fillScreen(0xF800);  // 赤色
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(0, 0);
    M5.Display.println("SD_MMC接続失敗");
    M5.Display.setCursor(0, 40);
    M5.Display.println("SPIモードを試してください");
  }
}

void loop() {
  if (sdcard_ok && schedules.size() > 0) {
    // 成功時の簡単なスケジュール表示
    M5.Display.fillScreen(0x07E0);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("SD_MMC動作中");
    M5.Display.setCursor(0, 30);
    M5.Display.println("起動から " + String(millis()/1000) + " 秒");
    
    // 最初の予定を表示
    if (schedules.size() > 0) {
      M5.Display.setCursor(0, 70);
      M5.Display.println("最初の予定:");
      M5.Display.setCursor(0, 100);
      M5.Display.println(schedules[0].action);
    }
  }
  
  delay(1000);
}