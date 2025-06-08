#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <CSV_Parser.h>
#include <Wire.h>
#include <ArtronShop_RX8130CE.h>

//==================== 定数・マクロ ====================
const char* SCHEDULE_FILE = "/schedule.csv";

// Tab5 RTC設定
ArtronShop_RX8130CE rtc(&Wire);
bool rtc_ok = false;
bool sdcard_ok = false;

const int SCREEN_WIDTH = 1280;
const int SCREEN_HEIGHT = 720;

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

//--- RTCの初期化 ---
void initRTC() {
  Serial.println("RTC初期化開始...");
  
  // Tab5のI2Cピン設定
  Wire.begin(31, 32);  // SDA=31, SCL=32
  
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

void initSDCard() {
  Serial.println("SD_MMCでSDカード初期化開始...");
  
  // SD_MMCモードで初期化（1ビットモード）
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
  
  auto cfg = M5.config();
  M5.begin(cfg);
  
  // 背景色設定
  M5.Display.setRotation(3);  // 横向き
  M5.Display.fillScreen(TFT_YELLOW);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setFont(&fonts::lgfxJapanGothic_40);
  M5.Display.setTextSize(1);
  
  // 初期化画面
  M5.Display.setCursor(50, 50);
  M5.Display.println("Tab5 スケジュール表示");
  M5.Display.setCursor(50, 100);
  M5.Display.println("初期化中...");

  // Wi-Fiを完全に無効化
  WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi無効化完了");

  // RTC初期化
  M5.Display.setCursor(50, 150);
  M5.Display.println("RTC初期化中...");
  initRTC();
  
  if (rtc_ok) {
    M5.Display.setCursor(50, 200);
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.println("RTC初期化成功");
  } else {
    M5.Display.setCursor(50, 200);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.println("RTC初期化失敗");
  }

  // SDカード初期化
  M5.Display.setCursor(50, 250);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.println("SDカード初期化中...");
  initSDCard();
  
  if (sdcard_ok) {
    M5.Display.setCursor(50, 300);
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.println("SDカード初期化成功");
    
    M5.Display.setCursor(50, 350);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.println("スケジュール読み込み中...");
    loadSchedules();
    
    if (schedules.size() > 0) {
      M5.Display.setCursor(50, 400);
      M5.Display.setTextColor(TFT_DARKGREEN);
      M5.Display.println("スケジュール読み込み成功: " + String(schedules.size()) + "件");
    } else {
      M5.Display.setCursor(50, 400);
      M5.Display.setTextColor(TFT_ORANGE);
      M5.Display.println("スケジュールが見つかりません");
    }
  } else {
    M5.Display.setCursor(50, 300);
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

void loop() {
  // シリアルからの時刻設定をチェック
  handleSerialTimeSet();  

  // 背景色
  M5.Display.fillScreen(TFT_YELLOW);

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

  //=== 画面描画 ===
  
  //--- タイトルと現在時刻（上部） ---
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setCursor(50, 20);
  M5.Display.println("Tab5 スケジュール表示");
  
  M5.Display.setTextSize(3);
  M5.Display.setCursor(50, 70);
  if (time_valid) {
    M5.Display.setTextColor(TFT_BLACK);
  } else {
    M5.Display.setTextColor(TFT_RED);
  }
  M5.Display.println("現在時刻:\n " + datetime);

  //--- ステータス表示 ---
  M5.Display.setTextSize(0.8);
  M5.Display.setCursor(900, 20);
  if (rtc_ok) {
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.print("RTC:OK  ");
  } else {
    M5.Display.setTextColor(TFT_RED);
    M5.Display.print("RTC:NG  ");
  }

  M5.Display.setCursor(900, 55);
  if (sdcard_ok) {
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.print("SDカード:OK  ");
  } else {
    M5.Display.setTextColor(TFT_RED);
    M5.Display.print("SDカード:NG  ");
  }

  M5.Display.setCursor(900, 90);
  M5.Display.setTextColor(TFT_BLUE);
  M5.Display.print("Wi-Fi:無効  ");

  M5.Display.setCursor(900, 125);
  if (schedules.size() > 0) {
    M5.Display.setTextColor(TFT_DARKGREEN);
    M5.Display.print("スケジュール:" + String(schedules.size()) + "件");
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
          struct tm start_tm = *temp1;  // 開始時刻をコピー
          
          struct tm* temp2 = localtime(&s.stop);
          struct tm stop_tm = *temp2;   // 終了時刻をコピー
          
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
    
    M5.Display.setTextSize(1.2);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setCursor(50, 310);
    M5.Display.println("【進行中の予定】");
    
    if (ongoing == 0) {
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(TFT_BLACK);
      M5.Display.setCursor(50, 360);
      M5.Display.println("現在進行中の予定はありません");
    } else {
      for (int i = 0; i < ongoing; ++i) {
        // 時刻
        M5.Display.setTextSize(0.9);
        M5.Display.setTextColor(TFT_PURPLE);
        M5.Display.setCursor(70, 360 + i * 70);
        M5.Display.println(ongoingTimes[i]);
      
        // 予定名
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_BLACK);
        M5.Display.setCursor(70, 360 + i * 70 + 30);
        M5.Display.println("・" + ongoingActions[i]);
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
        struct tm start_tm = *temp1;  // 開始時刻をコピー
        
        struct tm* temp2 = localtime(&s.stop);
        struct tm stop_tm = *temp2;   // 終了時刻をコピー
        
        char buf[32];
        sprintf(buf, "%02d:%02d～%02d:%02d", 
                start_tm.tm_hour, start_tm.tm_min, 
                stop_tm.tm_hour, stop_tm.tm_min);
        futureTimes[future] = String(buf);
        futureActions[future++] = s.action;
      }
    }
    
    M5.Display.setTextSize(1.2);
    M5.Display.setTextColor(TFT_BLUE);
    M5.Display.setCursor(690, 310);
    M5.Display.println("【今後の予定】");
    
    for (int i = 0; i < future && i < 4; ++i) {  // 最大4件表示
      // 時刻
      M5.Display.setTextSize(0.9);
      M5.Display.setTextColor(TFT_PURPLE);
      M5.Display.setCursor(710, 360 + i * 70);
      M5.Display.println(futureTimes[i]);
      
      // 予定名
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(TFT_BLACK);
      M5.Display.setCursor(710, 360 + i * 70 + 30);
      M5.Display.println("・" + futureActions[i]);
    }
    
    // 残りの予定がある場合
    if (future > 4) {
      M5.Display.setTextSize(0.8);
      M5.Display.setTextColor(TFT_DARKGRAY);
      M5.Display.setCursor(710, 670);
      M5.Display.println("...他 " + String(future - 6) + " 件");
    }
    
  } else {
    // エラーメッセージ表示
    M5.Display.setTextSize(2);
    M5.Display.setCursor(50, 310);
    if (!rtc_ok) {
      M5.Display.setTextColor(TFT_RED);
      M5.Display.println("RTCエラー");
      M5.Display.setCursor(50, 360);
      M5.Display.setTextSize(1);
      M5.Display.println("シリアルで時刻設定してください");
      M5.Display.setCursor(50, 400);
      M5.Display.println("例: 2025/06/14 08:30:00");
    } else if (!sdcard_ok) {
      M5.Display.setTextColor(TFT_RED);
      M5.Display.println("SDカードを確認してください");
    } else if (schedules.size() == 0) {
      M5.Display.setTextColor(TFT_ORANGE);
      M5.Display.println("スケジュールファイルを確認");
      M5.Display.setCursor(50, 360);
      M5.Display.setTextSize(1);
      M5.Display.println("・schedule.csvが存在するか");
      M5.Display.setCursor(50, 400);
      M5.Display.println("・CSV形式が正しいか");
    }
  }

  delay(1000);
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