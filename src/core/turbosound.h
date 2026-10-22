#pragma once

#include "core/ay.h"
#include "core/types.h"

namespace rainbow {

// The three AY-3-8912 chips of the Next. Port 0xFFFD still selects the active
// register, but a byte with bit 7 set selects a chip instead: bit 6 is its left
// output, bit 5 its right output, and bits 1:0 pick the chip. The data port
// 0xBFFD then talks to whichever chip was selected last.
//
// On the 128 and the +2A/+3 the same byte is an ordinary register number, so
// `setChipSelectEnabled(false)` keeps the classic decode and leaves the extra
// two chips silent.
class TurboSound {
public:
  static constexpr int ChipCount = 3;

  void reset();

  void setChipSelectEnabled(bool on) { chipSelect_ = on; }
  bool chipSelectEnabled() const { return chipSelect_; }

  // Port 0xFFFD.
  void select(u8 value);
  int selectedChip() const { return selected_; }

  // Port 0xBFFD.
  u8 read() const { return chips_[selected_].read(); }
  void write(u8 value) { chips_[selected_].write(value); }

  Ay38912& chip(int index);
  const Ay38912& chip(int index) const;

  // NextReg 0x08 bit 5. The arrangement is a property of the mixer, not of one
  // chip, so it is applied to all three.
  void setStereoMode(Ay38912::Stereo mode);
  Ay38912::Stereo stereoMode() const { return chips_[0].stereo(); }

  // NextReg 0x09 bits 7:5, one bit per chip: A+B+C goes to both outputs.
  void setChipMono(int index, bool mono);
  bool chipMono(int index) const;

  // NextReg 0x08 bit 1. With TurboSound off the two extra chips stop running,
  // which is what the hardware does with the AY that is not the default one.
  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }

  bool leftEnabled(int index) const;
  bool rightEnabled(int index) const;

  void tick(u32 cycles);
  float sampleLeft() const;
  float sampleRight() const;

  template <class Ar> void serialize(Ar& ar) {
    ar(chips_);
    ar(mono_);
    ar(left_);
    ar(right_);
    ar(selected_);
    ar(chipSelect_);
    ar(enabled_);
  }

private:
  static int clampIndex(int index);

  Ay38912 chips_[ChipCount];
  bool mono_[ChipCount]{};
  bool left_[ChipCount] = {true, true, true};
  bool right_[ChipCount] = {true, true, true};
  int selected_ = 0;
  bool chipSelect_ = false;
  bool enabled_ = false;
};

} // namespace rainbow
