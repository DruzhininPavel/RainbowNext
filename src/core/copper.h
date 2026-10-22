#pragma once

#include <array>

#include "core/types.h"

namespace rainbow {

// The Next's Copper: a small coprocessor that rewrites Next registers when a
// chosen raster line comes round. Its program lives in 2K of its own RAM, so it
// holds exactly 1024 two-byte instructions and its ten-bit program counter
// wraps back to the start.
//
// Two opcodes share one bit:
//   WAIT   1hhhhhhv vvvvvvvv   wait for raster line v at horizontal h*8
//   MOVE   0rrrrrrr vvvvvvvv   write value v to Next register r
// and two values are special cases: 0x0000 is a no-op and 0xFFFF is a wait for
// a line that never arrives, which is how a program halts.
//
// Register 0x61 holds the low eight bits of the write index, 0x62 holds its top
// three bits in bits 2:0 and the four control modes in bits 7:6:
//   00  stop, the program counter keeps its value
//   01  reset the program counter and start
//   10  start from the current program counter
//   11  reset and start, and reset again at the top of every frame
// A control write whose mode bits match the previous ones only moves the write
// index, so uploading a program does not restart it.
class Copper {
public:
  static constexpr int ProgramSize = 2048;
  static constexpr int InstructionCount = 1024;

  Copper() { reset(); }

  void reset();

  // Register 0x60: one byte of the program, the index auto-increments.
  void writeData8(u8 value);
  // Register 0x63: two writes, the high byte then the low byte, committed to
  // the program together so a half-written instruction is never executed.
  void writeData16(u8 value);
  // Register 0x61: the low eight bits of the write index.
  void writeAddressLsb(u8 value) { index_ = u16((index_ & 0x700) | value); }
  u8 addressLsb() const { return u8(index_ & 0xFF); }
  // Register 0x62: bits 2:0 finish the write index, bits 7:6 are the mode.
  void writeControl(u8 value);
  u8 control() const { return u8((mode_ << 6) | ((index_ >> 8) & 0x07)); }

  u8 mode() const { return mode_; }
  bool running() const { return mode_ != 0; }
  int programCounter() const { return cpc_; }
  void resetProgramCounter() { cpc_ = 0; }

  u16 instruction(int index) const {
    const size_t at = size_t(index & (InstructionCount - 1)) * 2;
    return u16((u16(program_[at]) << 8) | program_[at + 1]);
  }

  // Runs the program until it blocks on a WAIT for a later line. `write` is
  // called with a register number and a value for every MOVE.
  //
  // The compositor samples the register file once per scanline, so a WAIT
  // releases at the start of its target line and its horizontal position is
  // decoded but does not delay the instruction past that point.
  template <typename WriteRegister>
  void runToLine(int line, WriteRegister&& write) {
    if (mode_ == 0) return;
    // A program that never waits must not stall the frame.
    for (int budget = InstructionCount; budget > 0; --budget) {
      const u16 word = instruction(cpc_);
      if ((word & 0x8000) != 0) {
        const int target = word & 0x1FF;
        if (line < target) {
          waitLine_ = target;
          waitHpos_ = (word >> 9) & 0x3F;
          return;
        }
      } else if (word != 0x0000) {
        write(u8((word >> 8) & 0x7F), u8(word & 0xFF));
      }
      cpc_ = (cpc_ + 1) & (InstructionCount - 1);
    }
  }

  int waitLine() const { return waitLine_; }
  int waitHpos() const { return waitHpos_; }

  template <class Ar> void serialize(Ar& ar) {
    ar(program_);
    ar(index_);
    ar(mode_);
    ar(cpc_);
    ar(pendingMsb_);
    ar(waitLine_);
    ar(waitHpos_);
  }

private:
  std::array<u8, ProgramSize> program_{};
  u16 index_ = 0;
  u8 mode_ = 0;
  int cpc_ = 0;
  u8 pendingMsb_ = 0;
  int waitLine_ = 0;
  int waitHpos_ = 0;
};

} // namespace rainbow
