#include "core/nextreg.h"

namespace rainbow {

void NextRegFile::reset(bool hard) {
  lastResetHard_ = hard;

  // A hard reset wipes the file first; a soft reset is the FPGA's reset tree,
  // not a wipe. The VHDL only gives the registers below a reset branch, so
  // everything the firmware programmed before handing over -- the machine
  // configuration in 0x0A, the Layer 2 mapping in 0x80-0x8F, the palette in
  // 0x40-0x44 -- survives a soft reset. NextZXOS depends on that: it is handed
  // a configured machine and reads back the values it set rather than assuming
  // defaults. Power-on asserts the same branches, so both kinds run the list.
  if (hard) {
    values_.fill(0);
    selected_ = 0;
    // The FPGA powers up with both palettes parked on white in "write through
    // the old index" mode, and the boot ROM reads those back before it starts
    // programming colours.
    values_[0x85] = values_[0x89] = 0x8F;
    values_[0x86] = values_[0x87] = values_[0x88] = 0xFF;
  }

  values_[0x02] &= 0x80;                  // the ESP reset bit latches
  values_[0x03] &= 0x7F;                  // leave configuration mode
  values_[0x06] = u8((values_[0x06] & 0x5F) | 0xA0);
  values_[0x07] = 0x00;
  values_[0x08] &= 0x3F;
  values_[0x09] &= 0xEF;
  values_[0x0B] = 0x01;
  values_[0x10] = 0x04;                   // Layer 2: 256x192
  values_[0x12] = 0x08;                   // Layer 2 active bank
  values_[0x13] = 0x0B;                   // Layer 2 shadow bank
  values_[0x14] = 0xE3;                   // global transparency index
  values_[0x15] = values_[0x16] = values_[0x17] = 0x00;
  values_[0x22] = values_[0x23] = 0x00;
  values_[0x26] = values_[0x27] = 0x00;
  values_[0x2F] = 0x00;
  for (int reg = 0x30; reg <= 0x34; ++reg) values_[u8(reg)] = 0x00;
  values_[0x40] = 0x00;                   // palette index
  values_[0x42] = 0x07;                   // ULANext ink mask: eight and eight
  values_[0x43] = 0x00;
  values_[0x4A] = 0xE3;                   // fallback colour
  values_[0x4B] = 0xE3;                   // sprite transparency index
  values_[0x4C] = 0x0F;                   // tilemap transparency index
  values_[0x62] = 0x00;
  values_[0x64] = 0x00;
  values_[0x68] = 0x00;
  values_[0x6A] = values_[0x6B] = values_[0x6C] = 0x00;
  values_[0x6E] = 0x2C;                   // tilemap at 0x6C00 in bank 5
  values_[0x6F] = 0x0C;                   // tile definitions at 0x4C00
  values_[0x70] = values_[0x71] = 0x00;
  // The low nibble of the Layer 2 offset and of the fallback colour is
  // duplicated into the high one, so both halves of the byte agree.
  values_[0x80] = u8((values_[0x80] & 0x0F) * 0x11);
  values_[0x8C] = u8((values_[0x8C] & 0x0F) * 0x11);
  // A palette that is still in its "write through the old index" mode is
  // parked on its first colour, which is what the VHDL does so that the
  // first drawing after a reset is not in a random colour.
  if ((values_[0x85] & 0x80) != 0) {
    values_[0x82] = values_[0x83] = values_[0x84] = 0xFF;
    values_[0x85] |= 0x0F;
  }
  if ((values_[0x89] & 0x80) == 0) {
    values_[0x86] = values_[0x87] = values_[0x88] = 0xFF;
    values_[0x89] |= 0x0F;
  }
  for (int reg = 0x90; reg <= 0x93; ++reg) values_[u8(reg)] = 0x00;
  values_[0xA0] = 0x00;
  values_[0xA2] = 0x00;
  values_[0xA8] = 0x00;
  values_[0xA9] = 0x01;
  values_[0xC0] = 0x00;
  values_[0xC2] = values_[0xC3] = 0x00;
  values_[0xC4] = 0x81;                   // both Pi I/O lines float high
  values_[0xC5] = values_[0xC6] = 0x00;
  values_[0xCC] = values_[0xCD] = values_[0xCE] = 0x00;
  values_[0xD8] = values_[0xD9] = 0x00;

  // The divMMC entry points. The defaults hook 0x0000 and 0x0038 everywhere and
  // 0x0008 only while the +3 DOS ROM is paged, which is what esxDOS expects.
  values_[0xB8] = 0x83; // entry points 0: 0x0038 and 0x0000
  values_[0xB9] = 0x01; // ...0x0000 fires always, the rest only with ROM3 in
  values_[0xBA] = 0x00; // ...all of them map in after the instruction
  values_[0xBB] = 0xCD; // entry points 1: 0x3DXX, 0x0562, 0x04C6, NMI

  // The boot mapping: ROM at 0x0000, the two halves of bank 5 at 0x4000 (that
  // is the ULA screen), bank 2 at 0x8000 and banks 0/1 at 0xC000.
  static constexpr u8 Mmu[8] = {0xFF, 0xFF, 0x0A, 0x0B, 0x04, 0x05, 0x00, 0x01};
  for (int slot = 0; slot < 8; ++slot) values_[u8(0x50 + slot)] = Mmu[slot];
}

bool NextRegFile::isReadOnly(u8 reg) {
  switch (reg) {
    case 0x00: // machine ID
    case 0x01: // core version
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
    case 0x00: return 0x0A; // ZX Spectrum Next
    case 0x01: return 0x31; // core 3.01
    // Exactly one of bits 1:0 says how the machine last came up; the boot ROM
    // reads it to tell a cold start from an F4.
    case 0x02: return lastResetHard_ ? 0x02 : 0x01;
    case 0x0E: return 0x0A; // core 3.01.10
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
