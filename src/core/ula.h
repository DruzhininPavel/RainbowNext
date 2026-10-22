#pragma once

#include "core/keyboard.h"
#include "core/types.h"

namespace rainbow {

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

  // Timex SCLD register, written through port 0xFF. Bits 2:0 select the display
  // mode and bits 5:3 the two hi-res colours; bits 7:6 are the interrupt disable
  // and memory bank bits of the Timex Dock and are not modelled.
  void writeTimex(u8 value) { timex_ = value; }
  u8 timex() const { return timex_; }
  int screenMode() const { return timex_ & 0x07; }
  int hiresColour() const { return (timex_ >> 3) & 0x07; }
  bool hiRes() const { return screenMode() == 6; }

  // ULA+ control ports. 0xBF3B selects a register (bits 7:6 pick the palette or
  // the mode group, bits 5:0 the palette index) and 0xFF3B carries the data;
  // the palette itself lives in NextPalette because the Next shares it with the
  // ULA.
  void writeUlaPlusRegister(u8 value) { ulaPlusRegister_ = value; }
  u8 ulaPlusRegister() const { return ulaPlusRegister_; }
  void setUlaPlusMode(u8 value) { ulaPlusMode_ = value; }
  u8 ulaPlusMode() const { return ulaPlusMode_; }
  bool ulaPlusEnabled() const { return (ulaPlusMode_ & 0x01) != 0; }

private:
  u8 border_ = 0;
  u8 beeper_ = 0;
  u8 lastByte_ = 0;
  u8 defaultValue_ = 0xBF;
  u8 timex_ = 0;
  u8 ulaPlusRegister_ = 0;
  u8 ulaPlusMode_ = 0;
};

} // namespace rainbow
