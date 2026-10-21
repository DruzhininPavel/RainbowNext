#include "core/ula.h"

namespace mynext {

void Ula::reset(bool is128) {
  (void)is128; // the Issue 2/3 difference is not modelled yet, see docs/ROADMAP.md
  border_ = 0;
  beeper_ = 0;
  lastByte_ = 0;
  // Issue 3 boards return 0xBF with no keys pressed, i.e. only the EAR bit (6)
  // reads high because the tape input is idle. Both 48K and 128K use this value.
  defaultValue_ = 0xBF;
}

u8 Ula::read(u16 port, const Keyboard& keyboard) const {
  u8 value = defaultValue_;
  value = u8(value & keyboard.read(u8(port >> 8)));
  return value;
}

void Ula::write(u16 port, u8 value) {
  (void)port;
  lastByte_ = value;
  border_ = u8(value & 0x07);
  beeper_ = u8((value >> 4) & 0x01);
}

} // namespace mynext
