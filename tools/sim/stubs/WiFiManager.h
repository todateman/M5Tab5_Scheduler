#pragma once

class WiFiManager {
public:
  void setConfigPortalTimeout(int) {}
  bool autoConnect(const char*) { return true; }
};
