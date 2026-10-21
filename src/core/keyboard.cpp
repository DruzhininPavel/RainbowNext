#include "core/keyboard.h"

namespace mynext {

void Keyboard::reset() {
  for (auto& row : keys_) row.fill(false);
}

void Keyboard::setKey(int row, int column, bool down) {
  if (row < 0 || row >= Rows || column < 0 || column >= Columns) return;
  keys_[row][column] = down;
}

bool Keyboard::isDown(int row, int column) const {
  if (row < 0 || row >= Rows || column < 0 || column >= Columns) return false;
  return keys_[row][column];
}

u8 Keyboard::read(u8 high) const {
  u8 result = 0x1F;
  for (int row = 0; row < Rows; ++row) {
    if ((high & (1u << row)) == 0) {
      for (int bit = 0; bit < Columns; ++bit) {
        if (keys_[row][bit]) result = u8(result & ~(1u << bit));
      }
    }
  }
  return u8(result | 0xE0);
}

} // namespace mynext
