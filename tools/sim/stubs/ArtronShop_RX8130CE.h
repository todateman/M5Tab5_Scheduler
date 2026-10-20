// RTC の代わり。sim::rtc_base を起点に仮想 millis() で進む
#pragma once
#include "Arduino.h"
#include "Wire.h"

class ArtronShop_RX8130CE {
public:
  explicit ArtronShop_RX8130CE(TwoWire*) {}
  bool begin() { return true; }
  bool getTime(struct tm* t) {
    time_t now = sim::rtc_now();
    localtime_r(&now, t);
    return true;
  }
  bool setTime(struct tm t) {
    t.tm_isdst = -1;
    sim::rtc_base = mktime(&t) - sim::now_ms / 1000;
    return true;
  }
};
