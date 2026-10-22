#pragma once

#include "core/types.h"

namespace rainbow {

// AY-3-8912 programmable sound generator as wired into the Spectrum 128 and
// everything after it: three square-wave channels, one noise generator and one
// envelope generator, addressed through a register index port and a data port.
//
// The chip runs at half the CPU clock on every Spectrum, so `tick()` is called
// with CPU T-states and divides internally. Sample generation is left to the
// caller: `sampleLeft()`/`sampleRight()` return the level at the current
// instant, which keeps the chip free of any audio-device concerns.
class Ay38912 {
public:
  // The +2A/+3 wires channel A to the left, B to the centre and C to the right.
  // ABC and ACB name which of the outer channels goes left.
  enum class Stereo { Mono, ABC, ACB };

  void reset();

  // Register interface. The machine writes the index to one port and the data
  // to another, so selection and access are separate operations.
  void select(u8 reg) { selected_ = u8(reg & 0x0F); }
  u8 selected() const { return selected_; }
  u8 read() const { return readRegister(selected_); }
  u8 readRegister(u8 reg) const;
  void write(u8 value) { writeRegister(selected_, value); }
  void writeRegister(u8 reg, u8 value);

  // Advance the chip by `cycles` CPU T-states.
  void tick(u32 cycles);
  void setClockDivider(u32 divider) { divider_ = divider == 0 ? 1 : divider; }

  // Current output of one channel, 0..1, after tone/noise gating.
  float channel(u8 index) const;
  float sampleLeft() const;
  float sampleRight() const;
  // Mono downmix, what a single-speaker machine plays.
  float sampleMono() const { return (sampleLeft() + sampleRight()) * 0.5f; }

  void setStereo(Stereo mode) { stereo_ = mode; }
  Stereo stereo() const { return stereo_; }

  bool toneEnabled(u8 ch) const { return (regs_[7] & (1u << (ch & 3))) == 0; }
  bool noiseEnabled(u8 ch) const { return (regs_[7] & (8u << (ch & 3))) == 0; }
  u8 volume(u8 ch) const { return u8(regs_[8 + (ch & 3)] & 0x0F); }
  bool usingEnvelope(u8 ch) const { return (regs_[8 + (ch & 3)] & 0x10) != 0; }
  u8 envelopePosition() const { return u8(envPos_); }
  u8 envelopeShape() const { return regs_[13]; }
  bool envelopeRunning() const { return !envHolding_; }
  u32 tonePeriod(u8 ch) const { return tonePeriod_[ch & 3]; }
  u32 noisePeriod() const { return noisePeriod_; }
  u16 envelopePeriod() const { return envPeriod_; }

private:
  void clock();
  void stepNoise();
  void stepEnvelope();
  void reloadEnvelope();
  float channelLevel(u8 ch) const;

  u8 regs_[16]{};
  u8 selected_ = 0;

  u32 tonePeriod_[3]{};
  u32 toneCounter_[3]{};
  u8 toneOut_[3]{};

  u32 noisePeriod_ = 1;
  u32 noiseCounter_ = 1;
  u32 lfsr_ = 1;
  u8 noiseOut_ = 0;

  u16 envPeriod_ = 0;
  u32 envCounter_ = 0;
  i8 envPos_ = 15;
  i8 envDir_ = -1;
  u8 envStep_ = 0;
  bool envHolding_ = true;

  u32 divider_ = 2;
  u32 dividerAccum_ = 0;

  Stereo stereo_ = Stereo::Mono;
};

} // namespace rainbow
