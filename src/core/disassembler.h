#pragma once

#include <functional>
#include <string>

#include "core/types.h"

namespace rainbow {

// What an instruction does to the program counter, as far as a debugger needs
// to know. `Call` covers CALL and RST, the only instructions that can push a
// return address, so "step over" has to run until control comes back.
enum class Flow { Next, Jump, Call, Return };

struct Disassembled {
  int length = 1;
  std::string text;
  Flow flow = Flow::Next;
};

// Supplies the byte at an address exactly as the CPU would fetch it, so the
// listing follows the current paging.
using ByteReader = std::function<u8(u16)>;

// Decodes the instruction at `address`. When `z80n` is false the Next's extra
// ED opcodes are shown as the NOPs a classic Z80 executes.
Disassembled disassemble(u16 address, const ByteReader& read, bool z80n = false);

} // namespace rainbow
