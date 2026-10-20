// M5Unified の代わり。M5.Display は 1280x720 のスプライトで、ここに描かれた内容がスクリーンショットになる。
#pragma once

#include <M5GFX.h>
#include "Arduino.h"

class SimDisplay : public M5Canvas {
public:
  // 実機は縦置きパネルを rotation=3 で横向きにしているが、ここでは最初から 1280x720 で作る
  void setRotation(uint_fast8_t) {}
};

struct SimTouchDetail {
  int x = 0, y = 0;
  bool pressed = false, begin = false;
  bool wasPressed() const { return begin; }
  bool isPressed() const { return pressed; }
};

struct SimTouch {
  SimTouchDetail current;
  bool hasPending = false;
  int pendingX = 0, pendingY = 0;
  SimTouchDetail getDetail() const { return current; }
};

struct SimConfig {};

struct SimM5 {
  SimDisplay Display;
  SimTouch Touch;

  SimConfig config() { return {}; }
  void begin(SimConfig) {
    Display.setColorDepth(16);
    Display.createSprite(1280, 720);
  }
  // 予約されたタップを1回分の押下として出し、次の update で離す
  void update() {
    if (Touch.hasPending) {
      Touch.current = { Touch.pendingX, Touch.pendingY, true, true };
      Touch.hasPending = false;
    } else {
      Touch.current = {};
    }
  }
};
inline SimM5 M5;

namespace sim {
  inline void tap(int x, int y) {
    M5.Touch.hasPending = true;
    M5.Touch.pendingX = x;
    M5.Touch.pendingY = y;
  }
}
