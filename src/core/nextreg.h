#pragma once

#include <array>

#include "core/types.h"

namespace rainbow {

// The ZX Spectrum Next configuration space: 256 byte-wide registers addressed
// by number. Port 0x243B selects one, port 0x253B reads or writes it, and the
// Z80N NEXTREG instruction does the whole transaction in one opcode.
//
// This class only owns the storage and the register-level policy (which
// registers are read-only, what the read-only ones return). Everything that
// touches the rest of the machine -- resets, paging, the palette -- is handled
// by Machine.
class NextRegFile {
public:
  static constexpr int Count = 256;

  // MMU slot values 0-254 map that 8K page of RAM; 255 shows the ROM instead.
  static constexpr u8 RomPage = 0xFF;

  NextRegFile() { reset(true); }

  // Soft reset values, i.e. the state NextZXOS expects after an F4. Both kinds
  // of reset reload the register file; only the cause reported back through
  // register 0x02 differs.
  void reset(bool hard);

  u8 selected() const { return selected_; }
  void select(u8 reg) { selected_ = reg; }

  u8 read(u8 reg) const;
  void write(u8 reg, u8 value);
  static bool isReadOnly(u8 reg);

  u8 mmuSlot(int slot) const { return values_[u8(0x50 + (slot & 7))]; }
  void setMmuSlot(int slot, u8 page) { values_[u8(0x50 + (slot & 7))] = page; }

  // 0 = 3.5 MHz, 1 = 7 MHz, 2 = 14 MHz, 3 = 28 MHz.
  u8 turbo() const { return u8(values_[0x07] & 0x03); }

  template <class Ar> void serialize(Ar& ar) {
    ar(values_);
    ar(selected_);
    ar(lastResetHard_);
  }

private:
  std::array<u8, Count> values_{};
  u8 selected_ = 0;
  bool lastResetHard_ = true;
};

} // namespace rainbow
