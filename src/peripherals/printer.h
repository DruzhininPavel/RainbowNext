#pragma once

#include <string>
#include <vector>

#include "core/peripheral.h"

namespace rainbow {

// ZX Printer: a one-bit-per-dot spark printer on port 0xFB. Bit 7 is the dot,
// bit 2 runs the motor and the falling edge of bit 1 clocks it, so the printer
// advances one dot per strobe until the head reaches the right margin and the
// paper moves on a line. The image is kept so the frontend can save it.
class ZxPrinter : public Peripheral {
public:
  static constexpr int Width = 256;
  static constexpr int MaxHeight = 4096;

  ZxPrinter();

  const PeripheralInfo& info() const override { return info_; }
  bool ioWrite(u16 port, u8 value, u32 t) override;

  void reset() override;

  int height() const { return height_; }
  bool pixel(int x, int y) const;
  bool motor() const { return motor_; }
  // True once a dot has been printed since the last clear.
  bool used() const { return height_ > 0 || headX_ > 0; }
  void clear();

  // The printout as a binary PBM, which needs no image library.
  std::string toPbm() const;

private:
  void newLine();
  void setPixel(int x, int y, bool on);

  PeripheralInfo info_;
  // One bit per dot, 32 bytes per row.
  std::vector<u8> rows_;
  int height_ = 0;
  int headX_ = 0;
  bool motor_ = false;
  bool strobe_ = false;
};

} // namespace rainbow
