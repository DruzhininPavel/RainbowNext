#include "core/ula.h"

namespace mynext {

void Ula::reset(bool is128) {
  border_ = 0;
  beeper_ = 0;
  lastByte_ = 0;
  // Issue 3 boards return 0xBF with no keys pressed; the 128K machines use the
  // same value. The EAR bit (6) is supplied by the tape input, which is idle
  // high, so the default already reads as "no tape signal".
  defaultValue_ = is128 ? 0xBF : 0xBF;
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
