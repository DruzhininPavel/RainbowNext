#include "core/ay.h"

#include <algorithm>

namespace rainbow {

namespace {

// AY volume levels are logarithmic; this is the usual table of the 16 steps
// normalised to 0..1 (the same curve every accurate emulator uses).
constexpr float VolumeTable[16] = {
    0.0000f, 0.0046f, 0.0074f, 0.0110f, 0.0164f, 0.0244f, 0.0363f, 0.0541f,
    0.0806f, 0.1201f, 0.1789f, 0.2666f, 0.3972f, 0.5917f, 0.8814f, 1.0000f,
};

} // namespace

void Ay38912::reset() {
  regs_[0] = regs_[1] = regs_[2] = regs_[3] = 0;
  regs_[4] = regs_[5] = 0;
  regs_[6] = 0;
  regs_[7] = 0xFF; // all channels silent until the program enables them
  for (int i = 8; i < 16; ++i) regs_[i] = 0;
  selected_ = 0;

  for (int ch = 0; ch < 3; ++ch) {
    tonePeriod_[ch] = 1;
    toneCounter_[ch] = 1;
    toneOut_[ch] = 0;
  }
  noisePeriod_ = 1;
  noiseCounter_ = 1;
  lfsr_ = 1;
  noiseOut_ = 0;

  envPeriod_ = 0;
  envCounter_ = 1;
  envPos_ = 15;
  envDir_ = -1;
  envStep_ = 0;
  envHolding_ = true;

  dividerAccum_ = 0;
}

u8 Ay38912::readRegister(u8 reg) const {
  return regs_[reg & 0x0F];
}

void Ay38912::writeRegister(u8 reg, u8 value) {
  reg = u8(reg & 0x0F);
  regs_[reg] = value;

  switch (reg) {
    case 0: case 2: case 4: {
      const u8 ch = u8(reg / 2);
      tonePeriod_[ch] = u32(value) | (u32(regs_[reg + 1] & 0x0F) << 8);
      if (tonePeriod_[ch] == 0) tonePeriod_[ch] = 1;
      if (toneCounter_[ch] > tonePeriod_[ch]) toneCounter_[ch] = tonePeriod_[ch];
      break;
    }
    case 1: case 3: case 5: {
      const u8 ch = u8((reg - 1) / 2);
      tonePeriod_[ch] = u32(regs_[reg - 1]) | (u32(value & 0x0F) << 8);
      if (tonePeriod_[ch] == 0) tonePeriod_[ch] = 1;
      if (toneCounter_[ch] > tonePeriod_[ch]) toneCounter_[ch] = tonePeriod_[ch];
      break;
    }
    case 6:
      noisePeriod_ = value & 0x1F;
      if (noisePeriod_ == 0) noisePeriod_ = 1;
      break;
    case 11:
      envPeriod_ = u16(u32(value) | (u32(regs_[12]) << 8));
      break;
    case 12:
      envPeriod_ = u16(u32(regs_[11]) | (u32(value) << 8));
      break;
    case 13:
      reloadEnvelope();
      break;
    default:
      break;
  }
}

// A shape is four bits: hold, alternate, attack, continue. Attack starts the
// ramp at the top volume, everything else starts at the bottom.
void Ay38912::reloadEnvelope() {
  const u8 shape = regs_[13];
  envDir_ = (shape & 0x04) ? 1 : -1;
  envPos_ = (shape & 0x04) ? 0 : 15;
  envStep_ = 0;
  envHolding_ = false;
  envCounter_ = u32(envPeriod_) * 16;
  if (envCounter_ == 0) envCounter_ = 16;
}

// One 16-step pass has finished: decide what the next pass does. Shapes that
// do not continue simply hold the level they reached, which reproduces every
// one of the sixteen waveforms in the datasheet.
void Ay38912::stepEnvelope() {
  if (envHolding_) return;

  if (++envStep_ < 16) {
    envPos_ = i8(std::clamp<int>(envPos_ + envDir_, 0, 15));
    return;
  }

  envStep_ = 0;
  const u8 shape = regs_[13];
  const bool hold = (shape & 0x01) != 0;
  const bool alternate = (shape & 0x02) != 0;
  const bool continuous = (shape & 0x08) != 0;

  if (hold || !continuous) {
    envHolding_ = true;
    return;
  }
  if (alternate) {
    envDir_ = i8(-envDir_);
    envPos_ = i8(std::clamp<int>(envPos_ + envDir_, 0, 15));
    return;
  }
  envPos_ = envDir_ > 0 ? 0 : 15;
}

void Ay38912::stepNoise() {
  // 17-bit LFSR with taps on bits 0 and 3, as in the real chip.
  const u32 bit = (lfsr_ ^ (lfsr_ >> 3)) & 1u;
  lfsr_ = (lfsr_ >> 1) | (bit << 16);
  noiseOut_ = u8(lfsr_ & 1u);
}

void Ay38912::clock() {
  for (int ch = 0; ch < 3; ++ch) {
    if (--toneCounter_[ch] == 0) {
      toneCounter_[ch] = tonePeriod_[ch];
      toneOut_[ch] ^= 1;
    }
  }

  if (--noiseCounter_ == 0) {
    noiseCounter_ = noisePeriod_ * 2;
    stepNoise();
  }

  if (--envCounter_ == 0) {
    envCounter_ = u32(envPeriod_) * 16;
    if (envCounter_ == 0) envCounter_ = 16;
    stepEnvelope();
  }
}

void Ay38912::tick(u32 cycles) {
  dividerAccum_ += cycles;
  while (dividerAccum_ >= divider_) {
    dividerAccum_ -= divider_;
    clock();
  }
}

float Ay38912::channelLevel(u8 ch) const {
  const bool tone = toneOut_[ch] != 0 || !toneEnabled(ch);
  const bool noise = noiseOut_ != 0 || !noiseEnabled(ch);
  if (!tone || !noise) return 0.0f;

  const u8 amp = regs_[8 + ch];
  const u8 level = (amp & 0x10) ? u8(envPos_) : u8(amp & 0x0F);
  return VolumeTable[level & 0x0F];
}

float Ay38912::channel(u8 index) const {
  return channelLevel(u8(index & 3));
}

float Ay38912::sampleLeft() const {
  const float a = channelLevel(0);
  const float b = channelLevel(1);
  const float c = channelLevel(2);
  switch (stereo_) {
    case Stereo::Mono: return (a + b + c) / 3.0f;
    case Stereo::ABC: return (a + b * 0.5f) / 1.5f;
    case Stereo::ACB: return (a + c * 0.5f) / 1.5f;
  }
  return (a + b + c) / 3.0f;
}

float Ay38912::sampleRight() const {
  const float a = channelLevel(0);
  const float b = channelLevel(1);
  const float c = channelLevel(2);
  switch (stereo_) {
    case Stereo::Mono: return (a + b + c) / 3.0f;
    case Stereo::ABC: return (c + b * 0.5f) / 1.5f;
    case Stereo::ACB: return (b + c * 0.5f) / 1.5f;
  }
  return (a + b + c) / 3.0f;
}

} // namespace rainbow
