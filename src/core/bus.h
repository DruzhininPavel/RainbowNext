#pragma once

#include "core/types.h"

namespace mynext {

// The CPU talks to the rest of the machine exclusively through this interface.
// Keeping it virtual (rather than templated) keeps the core simple and makes it
// trivial to host the same core in the SDL frontend, in tests, or headless.
class Bus {
public:
  virtual ~Bus() = default;

  // Memory access. `t` is the machine clock (T-states since the start of the
  // current frame) at the beginning of the access.
  virtual u8 memRead(u16 addr, u32 t) = 0;
  virtual void memWrite(u16 addr, u8 value, u32 t) = 0;

  // I/O access.
  virtual u8 ioRead(u16 port, u32 t) = 0;
  virtual void ioWrite(u16 port, u8 value, u32 t) = 0;

  // Number of wait states the ULA inserts for a memory access that starts at
  // time `t`. Contended machines use this to slow the CPU down.
  virtual u8 contention(u16 addr, u32 t) = 0;

  // Advance the machine clock (ULA, audio, frame counter).
  virtual void tick(u32 cycles) = 0;
  virtual u32 now() const = 0;

  // Z80N only: the extended instruction set can write the Next register file
  // directly. A plain Spectrum has no such file, so the default is a no-op and
  // the same CPU core serves both machines.
  virtual void nextRegWrite(u8 reg, u8 value) {
    (void)reg;
    (void)value;
  }

  // Byte placed on the data bus during an interrupt acknowledge cycle.
  // The Spectrum ULA leaves 0xFF there, which is why IM 2 vectors on the
  // Spectrum always resolve to (I << 8) | 0xFF.
  virtual u8 interruptVector() { return 0xFF; }
};

} // namespace mynext
