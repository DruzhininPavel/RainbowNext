#pragma once

#include "core/keyboard.h"
#include "core/types.h"

namespace mynext {

// The ULA's externally visible state: border colour, beeper level and the
// EAR/MIC bits. Rendering and contention live in Machine because they need
// access to memory and the global clock.
class Ula {
public:
  void reset(bool is128);

  // Port 0xFE (A0 = 0).
  u8 read(u16 port, const Keyboard& keyboard) const;
  void write(u16 port, u8 value);

  u8 border() const { return border_; }
  u8 beeper() const { return beeper_; }
  u8 lastByte() const { return lastByte_; }

private:
  u8 border_ = 0;
  u8 beeper_ = 0;
  u8 lastByte_ = 0;
  u8 defaultValue_ = 0xBF;
};

} // namespace mynext
