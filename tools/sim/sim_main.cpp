// src/main.cpp をそのまま PC 上で動かし、各画面の擬似スクリーンショットを PNG で保存する。
//
//   tools/sim/build.sh を参照
//   ./tools/sim/build/tab5_sim [出力ディレクトリ] [SDカードのディレクトリ] ["YYYY/MM/DD HH:MM:SS"]
//
// 起動 → モード選択（Wi-Fi をタップ）→ 起動完了 → メイン画面 → 時刻設定（「戻る」をタップ）
// の順に実際の setup()/loop() を進め、それぞれの画面を撮影する。

#include "../../src/main.cpp"

#include <string>

namespace {

std::string outDir = "docs";

void saveScreenshot(const char* name) {
  size_t len = 0;
  void* png = M5.Display.createPng(&len, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  if (png == nullptr) {
    fprintf(stderr, "PNG の作成に失敗しました: %s\n", name);
    exit(1);
  }
  std::string path = outDir + "/" + name;
  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    fprintf(stderr, "書き込めません: %s\n", path.c_str());
    exit(1);
  }
  fwrite(png, 1, len, fp);
  fclose(fp);
  free(png);
  printf("%s (%zu bytes)\n", path.c_str(), len);
}

enum Phase { WAIT_MODE, WAIT_BOOT, WAIT_MAIN, WAIT_TIMESET, WAIT_RETURN, DONE };
Phase phase = WAIT_MODE;
uint32_t modeStart = 0;

// delay() の呼び出し元で今どの画面にいるかを判断する
//   モード選択: delay(10) / 起動完了: delay(2000) / メイン: delay(10) / 時刻設定: delay(15)
void onDelay(uint32_t ms) {
  switch (phase) {
    case WAIT_MODE:
      if (ms != 10) break;
      if (modeStart == 0) modeStart = millis();
      if (millis() - modeStart >= 1000) {           // カウントダウンが少し進んだところ
        saveScreenshot("screenshot_mode.png");
        sim::tap(160 + 230, 170 + 150);             // 「Wi-Fi接続」カード
        phase = WAIT_BOOT;
      }
      break;
    case WAIT_BOOT:
      if (ms == 2000) {
        saveScreenshot("screenshot_boot.png");
        phase = WAIT_MAIN;
      }
      break;
    case WAIT_MAIN:
      if (ms == 10) {
        saveScreenshot("screenshot_main.png");
        sim::tap(BTN_TIME_SET.x + BTN_TIME_SET.w / 2, BTN_TIME_SET.y + BTN_TIME_SET.h / 2);
        phase = WAIT_TIMESET;
      }
      break;
    case WAIT_TIMESET:
      if (ms == 15) {
        saveScreenshot("screenshot_timeset.png");
        sim::tap(288 + 120, 628 + 36);              // 「戻る」
        phase = WAIT_RETURN;
      }
      break;
    case WAIT_RETURN:
      if (ms == 10) phase = DONE;                   // メイン画面に戻った
      break;
    case DONE:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) outDir = argv[1];
  if (argc > 2) sim::sd_root = argv[2];
  const char* start = (argc > 3) ? argv[3] : "2026/10/24 13:50:00";

  setenv("TZ", "JST-9", 1);
  tzset();

  struct tm t = {};
  if (sscanf(start, "%d/%d/%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
    fprintf(stderr, "時刻の形式が正しくありません: %s\n", start);
    return 1;
  }
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  t.tm_isdst = -1;
  sim::rtc_base = mktime(&t);
  sim::on_delay = onDelay;

  setup();
  for (int i = 0; i < 100000 && phase != DONE; i++) {
    loop();
  }
  if (phase != DONE) {
    fprintf(stderr, "すべての画面を撮影できませんでした (phase=%d)\n", phase);
    return 1;
  }
  return 0;
}
