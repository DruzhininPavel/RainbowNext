#include "core/turbosound.h"

namespace mynext {

int TurboSound::clampIndex(int index) {
  if (index < 0) return 0;
  if (index >= ChipCount) return ChipCount - 1;
  return index;
}

void TurboSound::reset() {
  for (Ay38912& chip : chips_) chip.reset();
  for (bool& mono : mono_) mono = false;
  for (bool& on : left_) on = true;
  for (bool& on : right_) on = true;
  selected_ = 0;
  enabled_ = false;
}

void TurboSound::select(u8 value) {
  // Only the Next gives bit 7 a meaning; on the older machines the whole byte
  // is a register number and the chip below is always the first one.
  if (chipSelect_ && (value & 0x80) != 0) {
    // The datasheet numbers the chips from the top of the field: 11 is the
    // first one, 10 the second and 01 the third. 00 is the value the machine
    // powers up with, so it has to mean the first chip as well.
    const u8 bits = u8(value & 0x03);
    selected_ = bits == 0x02 ? 1 : (bits == 0x01 ? 2 : 0);
    left_[selected_] = (value & 0x40) != 0;
    right_[selected_] = (value & 0x20) != 0;
    return;
  }
  chips_[selected_].select(value);
}

Ay38912& TurboSound::chip(int index) { return chips_[clampIndex(index)]; }

const Ay38912& TurboSound::chip(int index) const { return chips_[clampIndex(index)]; }

void TurboSound::setStereoMode(Ay38912::Stereo mode) {
  for (Ay38912& chip : chips_) chip.setStereo(mode);
}

void TurboSound::setChipMono(int index, bool mono) {
  mono_[clampIndex(index)] = mono;
}

bool TurboSound::chipMono(int index) const { return mono_[clampIndex(index)]; }

bool TurboSound::leftEnabled(int index) const { return left_[clampIndex(index)]; }

bool TurboSound::rightEnabled(int index) const { return right_[clampIndex(index)]; }

void TurboSound::tick(u32 cycles) {
  chips_[0].tick(cycles);
  if (!enabled_) return;
  chips_[1].tick(cycles);
  chips_[2].tick(cycles);
}

float TurboSound::sampleLeft() const {
  float sum = 0.0f;
  for (int i = 0; i < ChipCount; ++i) {
    if (!left_[i]) continue;
    sum += mono_[i] ? chips_[i].sampleMono() : chips_[i].sampleLeft();
  }
  return sum;
}

float TurboSound::sampleRight() const {
  float sum = 0.0f;
  for (int i = 0; i < ChipCount; ++i) {
    if (!right_[i]) continue;
    sum += mono_[i] ? chips_[i].sampleMono() : chips_[i].sampleRight();
  }
  return sum;
}

} // namespace mynext
