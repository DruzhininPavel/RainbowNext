#pragma once

#include "core/peripheral.h"

namespace rainbow {

// Kempston mouse. Three read-only ports carry the buttons and the two
// quadrature counters; the interface itself does the counting, so a program
// only sees the running X and Y.
class KempstonMouse : public Peripheral {
public:
  KempstonMouse();

  const PeripheralInfo& info() const override { return info_; }
  bool ioRead(u16 port, u32 t, u8& value) override;

  // Relative movement in mouse units; the counters wrap around.
  void move(int dx, int dy);
  void setPosition(u8 x, u8 y) {
    x_ = x;
    y_ = y;
  }
  void setButtons(u8 buttons) { buttons_ = buttons & 0x07; }
  u8 buttons() const { return buttons_; }
  u8 x() const { return x_; }
  u8 y() const { return y_; }
  void reset() override {
    x_ = y_ = 0;
    buttons_ = 0;
  }

private:
  PeripheralInfo info_;
  u8 x_ = 0;
  u8 y_ = 0;
  u8 buttons_ = 0;
};

} // namespace rainbow
