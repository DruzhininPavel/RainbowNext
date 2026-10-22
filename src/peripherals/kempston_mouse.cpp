#include "peripherals/kempston_mouse.h"

namespace rainbow {

KempstonMouse::KempstonMouse() {
  info_.id = "kempston-mouse";
  info_.name = "Kempston Mouse";
  info_.version = "1.0";
  info_.author = "RainbowNext";
  info_.description =
      "Three read-only ports: buttons on 0xFBDF, X on 0xFFDF and Y on 0xFDFF.";
  info_.ports = {0xFBDF, 0xFFDF, 0xFDFF};
}

bool KempstonMouse::ioRead(u16 port, u32, u8& value) {
  switch (port) {
    case 0xFBDF:
      // Bit 0 right, 1 left, 2 middle, the upper bits float high.
      value = u8(~buttons_ & 0x07) | 0xF8;
      return true;
    case 0xFFDF:
      value = x_;
      return true;
    case 0xFDFF:
      value = y_;
      return true;
    default:
      return false;
  }
}

void KempstonMouse::move(int dx, int dy) {
  // The counters are 8-bit, so a fast drag wraps exactly as the hardware does.
  x_ = u8((int(x_) + dx) & 0xFF);
  y_ = u8((int(y_) - dy) & 0xFF);
}

} // namespace rainbow
