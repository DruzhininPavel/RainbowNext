#include "core/dac.h"

namespace mynext {

namespace {

// A DAC parks its value on one side of the mix. A and B are the left pair, C
// and D the right one; a port that reaches both sides is the centre.
float level(u8 value) { return (float(value) - 128.0f) / 128.0f; }

constexpr int A = 0;
constexpr int B = 1;
constexpr int C = 2;
constexpr int D = 3;

} // namespace

void SpecDrum::reset() {
  for (u8& dac : dacs_) dac = 0x80;
}

bool SpecDrum::handles(u16 port) {
  switch (port & 0x00FF) {
    case 0x0F: case 0x1F: case 0x3F: case 0x4F: case 0x5F:
    case 0xB3: case 0xDF: case 0xF1: case 0xF3: case 0xF9: case 0xFB:
      return true;
    default:
      return false;
  }
}

void SpecDrum::write(u16 port, u8 value) {
  switch (port & 0x00FF) {
    case 0x1F: case 0xF1: case 0x3F: writeDac(A, value); return;
    case 0x0F: case 0xF3: writeDac(B, value); return;
    case 0xDF: case 0xFB: writeDac(A, value); writeDac(D, value); return;
    case 0xB3: writeDac(B, value); writeDac(C, value); return;
    case 0x4F: case 0xF9: writeDac(C, value); return;
    case 0x5F: writeDac(D, value); return;
    default: return;
  }
}

void SpecDrum::writeDac(int index, u8 value) {
  if (index < 0 || index >= DacCount) return;
  dacs_[index] = value;
}

u8 SpecDrum::dac(int index) const {
  if (index < 0 || index >= DacCount) return 0x80;
  return dacs_[index];
}

float SpecDrum::sampleLeft() const { return (level(dacs_[A]) + level(dacs_[B])) * 0.5f; }

float SpecDrum::sampleRight() const { return (level(dacs_[C]) + level(dacs_[D])) * 0.5f; }

} // namespace mynext
