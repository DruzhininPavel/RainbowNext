#pragma once

#include <array>

#include "core/types.h"

namespace rainbow {

// The ZX Spectrum keyboard is an 8x5 matrix. A row is selected by pulling one
// of the address lines A8..A15 low; the five data bits report the keys in that
// row (0 = pressed).
class Keyboard {
public:
  static constexpr int Rows = 8;
  static constexpr int Columns = 5;

  void reset();
  void setKey(int row, int column, bool down);
  // A second layer for the keys a gamepad stands in for. It is kept apart from
  // the matrix so that letting go of the pad never clears a key the keyboard is
  // still holding, and it stays out of the save state: like the Kempston byte it
  // is live host input rather than machine state.
  void setPadKey(int row, int column, bool down);
  bool isDown(int row, int column) const;

  // `high` is the high byte of the I/O port address. Returns 0xFF for bits
  // that no selected row drives, and 0 for pressed keys.
  u8 read(u8 high) const;

  template <class Ar> void serialize(Ar& ar) { ar(keys_); }

private:
  std::array<std::array<bool, Columns>, Rows> keys_{};
  std::array<std::array<bool, Columns>, Rows> padKeys_{};
};

} // namespace rainbow
