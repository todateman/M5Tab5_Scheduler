#pragma once
#include "Arduino.h"

#define WIFI_OFF 0

struct IPAddress {
  String toString() const { return "192.168.1.10"; }
};

struct SimWiFi {
  void setPins(int, int, int, int, int, int, int) {}
  void mode(int) {}
  IPAddress localIP() const { return {}; }
};
inline SimWiFi WiFi;
