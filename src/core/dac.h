#pragma once

#include "core/types.h"

namespace rainbow {

// The four 8-bit DACs of the Next. A write parks a sample on a DAC and it stays
// there until the next write, so a program feeds them from the DMA, the Copper
// or a CTC interrupt and never touches them in its main loop.
//
// Every DAC has a fixed panning -- A and B to the left, C and D to the right --
// and the ports are wired to one or two of them, which is what turns a single
// `OUT (0xDF)` into the centre channel a SpecDrum tape expects.
class SpecDrum {
public:
  static constexpr int DacCount = 4;

  void reset();

  // True for the low bytes the DACs answer on: 0x0F, 0x1F, 0x3F, 0x4F, 0x5F,
  // 0xB3, 0xDF, 0xF1, 0xF3, 0xF9 and 0xFB.
  static bool handles(u16 port);

  void write(u16 port, u8 value);
  void writeDac(int index, u8 value);
  u8 dac(int index) const;

  float sampleLeft() const;
  float sampleRight() const;

private:
  u8 dacs_[DacCount] = {0x80, 0x80, 0x80, 0x80};
};

} // namespace rainbow
