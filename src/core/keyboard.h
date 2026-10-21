#pragma once

#include <array>

#include "core/types.h"

namespace mynext {

// The ZX Spectrum keyboard is an 8x5 matrix. A row is selected by pulling one
// of the address lines A8..A15 low; the five data bits report the keys in that
// row (0 = pressed).
class Keyboard {
public:
  static constexpr int Rows = 8;
  static constexpr int Columns = 5;

  void reset();
  void setKey(int row, int column, bool down);
  bool isDown(int row, int column) const;

  // `high` is the high byte of the I/O port address. Returns 0xFF for bits
  // that no selected row drives, and 0 for pressed keys.
  u8 read(u8 high) const;

private:
  std::array<std::array<bool, Columns>, Rows> keys_{};
};

} // namespace mynext
