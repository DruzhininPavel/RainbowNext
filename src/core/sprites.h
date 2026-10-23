#pragma once

#include <array>

#include "core/types.h"

namespace rainbow {

// The Next's sprite hardware: 128 sprites of 16x16 pixels drawn over a
// 320x256 surface (the ULA screen plus a 32 pixel border on every side). Each
// sprite carries four or five attribute bytes and reads its pattern from a
// separate 16K pattern RAM, so the attributes can be shuffled without moving
// any pixels.
//
// The five attribute bytes are:
//   0  X LSB
//   1  Y LSB
//   2  PPPP XM YM R X8          palette offset, mirror, rotate, X bit 8
//   3  V E N5 N4 N3 N2 N1 N0    visible, extended (byte 4 present), pattern
//   4  H N6 T XX YY Y8          pattern size, pattern bit 6, X/Y scale, Y bit 8
// Without byte 4 the sprite is an 8-bit, unscaled, 16x16 sprite.
//
// Byte 4 with its top two bits at 01 marks a *relative* sprite, which hangs
// off the last anchor seen while the list is scanned from sprite 0 upwards.
// The two encodings are:
//   anchor     H N6 T XX YY Y8
//   relative   0 1  N6 XX YY PO
// A relative sprite keeps its position in bytes 0 and 1 as a signed offset
// from the anchor, inherits the anchor's pattern size and can add the anchor's
// pattern (PO) and palette offset (PR, bit 0 of byte 2) to its own. An anchor
// with T set groups its relatives into a single big sprite that follows the
// anchor's mirror, rotate and scale.
struct SpriteState {
  bool visible = false;
  int x = 0; // nine bits, wrapping at 512
  int y = 0;
  int pattern = 0;
  int paletteOffset = 0;
  bool fourBit = false;
  bool mirrorX = false;
  bool mirrorY = false;
  bool rotate = false;
  int scaleX = 1;
  int scaleY = 1;
};

class Sprites {
public:
  static constexpr int Count = 128;
  static constexpr int Size = 16;
  static constexpr int PatternRamSize = 16384;
  // One pattern is 128 bytes: sixteen rows of eight bytes. A 4-bit pattern
  // fills them all, an 8-bit pattern uses two neighbouring slots.
  static constexpr int PatternStride = 128;
  static constexpr int SurfaceWidth = 320;
  static constexpr int SurfaceHeight = 256;

  Sprites() { reset(); }

  void reset();

  // Register 0x34. Bit 7 only matters when it is in lockstep with port 0x303B
  // (register 0x09 bit 4).
  void setNumber(u8 value) { number_ = value; }
  u8 number() const { return number_ & 0x7F; }
  void setLockstep(bool on) { lockstep_ = on; }
  bool lockstep() const { return lockstep_; }

  // Port 0x303B: bits 6:0 pick the sprite whose attributes follow, bit 7 adds
  // 128 to the pattern write address.
  void selectPort(u8 value);
  u8 portNumber() const { return portNumber_; }
  bool patternOffset() const { return patternOffset_; }

  // Registers 0x35-0x39, or 0x75-0x79 when the number auto-increments.
  void writeAttribute(int index, u8 value, bool autoIncrement);

  // Port 0x57: the attribute bytes streamed in order. The sequence restarts
  // after four bytes unless attribute 3 asks for the fifth.
  void writeAttributePort(u8 value);

  // Port 0x5B: pattern bytes for the selected pattern.
  void writePattern(u8 value);

  // Register 0x19 clip window.
  void writeClip(u8 value);
  void resetClipIndex() { clipIndex_ = 0; }
  const std::array<u8, 4>& clip() const { return clip_; }
  u8 clipIndex() const { return clipIndex_; }

  // Port 0x303B read: bit 1 reports that the per-line sprite budget ran out,
  // bit 0 that two sprites drew over each other. Reading clears both.
  void clearStatus() {
    collision_ = false;
    overflow_ = false;
  }
  u8 readStatus() const { return u8((overflow_ ? 0x02 : 0x00) | (collision_ ? 0x01 : 0x00)); }
  void noteCollision() { collision_ = true; }
  void noteOverflow() { overflow_ = true; }
  bool collided() const { return collision_; }
  bool overflowed() const { return overflow_; }

  template <class Ar> void serialize(Ar& ar) {
    ar(attrs_);
    ar(pattern_);
    ar(clip_);
    ar(number_);
    ar(portNumber_);
    ar(patternOffset_);
    ar(attributeIndex_);
    ar(clipIndex_);
    ar(patternAddress_);
    ar(lockstep_);
    ar(collision_);
    ar(overflow_);
  }

  u8 attribute(int sprite, int index) const {
    return attrs_[size_t(sprite & 0x7F)][size_t(index)];
  }

  bool visible(int sprite) const { return (attribute(sprite, 3) & 0x80) != 0; }
  bool extended(int sprite) const { return (attribute(sprite, 3) & 0x40) != 0; }
  // Top two bits of byte 4 at 01: the sprite hangs off the preceding anchor.
  bool relative(int sprite) const {
    return extended(sprite) && (attribute(sprite, 4) & 0xC0) == 0x40;
  }
  // Byte 4 bit 5 of an anchor: 1 groups its relatives into a single big
  // sprite, 0 leaves them as independent composite sprites.
  bool unified(int sprite) const {
    return extended(sprite) && (attribute(sprite, 4) & 0x20) != 0;
  }
  // Pattern number 0-127; the hardware stores it as N5..N0 with N6 as the
  // least significant bit, hence the odd ordering. A relative sprite keeps N6
  // in bit 5, where an anchor has its unified flag.
  int pattern(int sprite) const {
    const u8 a3 = attribute(sprite, 3);
    int n6 = 0;
    if (relative(sprite)) {
      n6 = (attribute(sprite, 4) >> 5) & 0x01;
    } else if (extended(sprite)) {
      n6 = (attribute(sprite, 4) >> 6) & 0x01;
    }
    return ((a3 & 0x3F) << 1) | n6;
  }
  // Four-bit patterns only exist in the extended form; relative sprites take
  // the size from their anchor, so this is only meaningful for anchors.
  bool fourBit(int sprite) const {
    return extended(sprite) && !relative(sprite) && (attribute(sprite, 4) & 0x80) != 0;
  }
  int x(int sprite) const {
    return attribute(sprite, 0) | ((attribute(sprite, 2) & 0x01) << 8);
  }
  int y(int sprite) const {
    const int high = (extended(sprite) && !relative(sprite))
                         ? (attribute(sprite, 4) & 0x01)
                         : 0;
    return attribute(sprite, 1) | (high << 8);
  }
  int paletteOffset(int sprite) const { return attribute(sprite, 2) >> 4; }
  bool mirrorX(int sprite) const { return (attribute(sprite, 2) & 0x08) != 0; }
  bool mirrorY(int sprite) const { return (attribute(sprite, 2) & 0x04) != 0; }
  bool rotate(int sprite) const { return (attribute(sprite, 2) & 0x02) != 0; }
  // The scale bits only exist in the extended form, where they mean 1, 2, 4
  // or 8 times.
  int scaleX(int sprite) const {
    return extended(sprite) ? (1 << ((attribute(sprite, 4) >> 3) & 0x03)) : 1;
  }
  int scaleY(int sprite) const {
    return extended(sprite) ? (1 << ((attribute(sprite, 4) >> 1) & 0x03)) : 1;
  }

  // The sprite's own attributes as an anchor would present them.
  SpriteState anchorState(int sprite) const;
  // The same, with every anchor-dependent property resolved.
  SpriteState relativeState(int sprite, const SpriteState& anchor,
                            bool unified) const;
  // Every sprite in list order, each resolved against the anchor that
  // precedes it.
  std::array<SpriteState, Count> resolve() const;

  // The pattern colour index of one cell of a sprite, already mirrored and
  // rotated, or -1 when the cell is the transparent index.
  int pixel(const SpriteState& state, int column, int row,
            u8 transparency) const;
  int pixel(int sprite, int column, int row, u8 transparency) const {
    return pixel(resolve()[size_t(sprite & 0x7F)], column, row, transparency);
  }

  u8 patternByte(u16 address) const { return pattern_[size_t(address & 0x3FFF)]; }

private:
  u8 patternForWrite() const { return u8(portNumber_ * 2 + (patternOffset_ ? 1 : 0)); }

  std::array<std::array<u8, 5>, Count> attrs_{};
  std::array<u8, PatternRamSize> pattern_{};
  std::array<u8, 4> clip_ = {0, 255, 0, 191};
  u8 number_ = 0;
  u8 portNumber_ = 0;
  u8 patternOffset_ = 0;
  u8 attributeIndex_ = 0;
  u8 clipIndex_ = 0;
  u16 patternAddress_ = 0;
  bool lockstep_ = false;
  bool collision_ = false;
  bool overflow_ = false;
};

} // namespace rainbow
