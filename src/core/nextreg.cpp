#include "core/nextreg.h"

namespace rainbow {

void NextRegFile::reset(bool hard) {
  values_.fill(0);
  selected_ = 0;
  lastResetHard_ = hard;

  values_[0x03] = 0x00; // machine type: configuration mode
  values_[0x06] = 0xA0; // the F3 and F8 hotkeys are enabled after a soft reset
  values_[0x12] = 0x00; // Layer 2 active bank
  values_[0x13] = 0x00; // Layer 2 shadow bank
  values_[0x14] = 0xE3; // global transparency index
  values_[0x42] = 0x07; // ULANext ink mask: eight inks, eight papers
  values_[0x4A] = 0xE3; // fallback colour
  values_[0x4B] = 0xE3; // sprite transparency index
  values_[0x4C] = 0x0F; // tilemap transparency index
  values_[0x6E] = 0x6C; // tilemap at 0x6C00 in bank 5
  values_[0x6F] = 0x4C; // tile definitions at 0x4C00 in bank 5

  // The boot mapping: ROM at 0x0000, the two halves of bank 5 at 0x4000 (that
  // is the ULA screen), bank 2 at 0x8000 and banks 0/1 at 0xC000.
  static constexpr u8 Mmu[8] = {0xFF, 0xFF, 0x0A, 0x0B, 0x04, 0x05, 0x00, 0x01};
  for (int slot = 0; slot < 8; ++slot) values_[u8(0x50 + slot)] = Mmu[slot];
}

bool NextRegFile::isReadOnly(u8 reg) {
  switch (reg) {
    case 0x00: // machine ID
    case 0x01: // core version
    case 0x08: // peripheral 3
    case 0x0E: // core sub-version
    case 0x0F:
    case 0x1C: // active raster line
    case 0x1D:
    case 0x1E: // active video line
    case 0x1F:
      return true;
    default:
      return false;
  }
}

u8 NextRegFile::read(u8 reg) const {
  switch (reg) {
    case 0x00: return 0x08; // an emulator, not a DE1A/DE2A board
    case 0x01: return 0x30; // core 3.0
    // Exactly one of bits 1:0 says how the machine last came up; the boot ROM
    // reads it to tell a cold start from an F4.
    case 0x02: return lastResetHard_ ? 0x02 : 0x01;
    case 0x08: return 0x00;
    case 0x0E: return 0x00;
    case 0x0F: return 0x00;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: return 0x00;
    default: return values_[reg];
  }
}

void NextRegFile::write(u8 reg, u8 value) {
  if (isReadOnly(reg)) return;
  values_[reg] = value;
}

} // namespace rainbow
