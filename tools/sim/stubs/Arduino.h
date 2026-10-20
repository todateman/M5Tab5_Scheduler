// PC 上で src/main.cpp の描画処理を動かすための Arduino 互換スタブ。
// 時刻・タッチ・RTC・SD は sim:: の値で駆動する（tools/sim/sim_main.cpp から操作）。
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <sys/time.h>
#include <vector>

namespace sim {
  inline uint32_t now_ms = 0;                            // 仮想 millis()
  inline time_t rtc_base = 0;                            // millis()=0 時点の RTC 時刻
  inline std::string sd_root = "microsd";                // SD カードとして扱うディレクトリ
  inline std::function<void(uint32_t)> on_delay;         // delay() のたびに呼ばれる
  inline bool quiet = true;                              // Serial 出力を捨てる

  inline time_t rtc_now() { return rtc_base + now_ms / 1000; }
}

class String {
  std::string s_;
public:
  String() = default;
  String(const char* c) : s_(c ? c : "") {}
  String(const std::string& s) : s_(s) {}
  String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned int v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  String(long long v) : s_(std::to_string(v)) {}
  String(unsigned long long v) : s_(std::to_string(v)) {}

  const char* c_str() const { return s_.c_str(); }
  // LovyanGFX の const char* 版 API（textWidth など）にそのまま渡せるようにする（PC 版のみ）
  operator const char*() const { return s_.c_str(); }
  unsigned int length() const { return (unsigned int)s_.size(); }

  char operator[](int i) const { return s_[(size_t)i]; }
  char& operator[](int i) { return s_[(size_t)i]; }

  void remove(int index) { if ((size_t)index < s_.size()) s_.erase((size_t)index); }
  void remove(int index, int count) { if ((size_t)index < s_.size()) s_.erase((size_t)index, (size_t)count); }
  bool startsWith(const char* p) const { return s_.rfind(p, 0) == 0; }
  void trim() {
    const char* ws = " \t\r\n";
    size_t b = s_.find_first_not_of(ws);
    if (b == std::string::npos) { s_.clear(); return; }
    s_ = s_.substr(b, s_.find_last_not_of(ws) - b + 1);
  }

  bool operator==(const char* o) const { return s_ == o; }
  bool operator==(const String& o) const { return s_ == o.s_; }

  String& operator+=(char c) { s_ += c; return *this; }
  String& operator+=(const char* o) { s_ += o; return *this; }
  String& operator+=(const String& o) { s_ += o.s_; return *this; }

  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + b); }
  friend String operator+(const char* a, const String& b) { return String(a + b.s_); }
};

struct SimSerial {
  void begin(unsigned long) {}
  int available() { return 0; }
  int read() { return -1; }
  void flush() {}
  size_t write(const uint8_t*, size_t n) { return n; }
  void print(const char* s) { if (!sim::quiet) fputs(s, stdout); }
  void println(const char* s = "") { if (!sim::quiet) { fputs(s, stdout); fputc('\n', stdout); } }
  void print(const String& s) { print(s.c_str()); }
  void println(const String& s) { println(s.c_str()); }
  int printf(const char* fmt, ...) {
    if (sim::quiet) return 0;
    va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
  }
};
inline SimSerial Serial;

inline uint32_t millis() { return sim::now_ms; }
inline void delay(uint32_t ms) {
  sim::now_ms += ms;
  if (sim::on_delay) sim::on_delay(ms);
}

template <typename T, typename L, typename H>
inline T constrain(T x, L lo, H hi) { return x < (T)lo ? (T)lo : (x > (T)hi ? (T)hi : x); }

inline bool getLocalTime(struct tm* info, uint32_t = 5000) {
  time_t t = sim::rtc_now();
  localtime_r(&t, info);
  return true;
}
inline void configTzTime(const char* tz, const char*, const char* = nullptr, const char* = nullptr) {
  setenv("TZ", tz, 1);
  tzset();
}

enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
