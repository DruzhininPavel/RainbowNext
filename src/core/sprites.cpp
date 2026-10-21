#include "core/sprites.h"

namespace mynext {

void Sprites::reset() {
  for (auto& attr : attrs_) attr.fill(0);
  pattern_.fill(0);
  clip_ = {0, 255, 0, 191};
  number_ = 0;
  portNumber_ = 0;
  patternOffset_ = 0;
  attributeIndex_ = 0;
  clipIndex_ = 0;
  patternAddress_ = 0;
}

void Sprites::selectPort(u8 value) {
  portNumber_ = u8(value & 0x7F);
  patternOffset_ = (value & 0x80) != 0;
  // A fresh selection restarts both the attribute stream and the pattern
  // write address, which is what makes "select, then dump bytes" work.
  attributeIndex_ = 0;
  patternAddress_ = u16(portNumber_ * 2 * PatternStride);
  if (patternOffset_) patternAddress_ = u16(patternAddress_ + PatternStride);
  if (lockstep_) number_ = portNumber_;
}

void Sprites::writeAttribute(int index, u8 value, bool autoIncrement) {
  if (index < 0 || index > 4) return;
  const int sprite = number() & 0x7F;
  attrs_[size_t(sprite)][size_t(index)] = value;
  if (autoIncrement) {
    const u8 next = u8((sprite + 1) & 0x7F);
    number_ = next;
    if (lockstep_) portNumber_ = next;
  }
}

void Sprites::writeAttributePort(u8 value) {
  const int sprite = portNumber_ & 0x7F;
  attrs_[size_t(sprite)][size_t(attributeIndex_)] = value;
  // Four bytes is the default; the fifth only follows an extended sprite.
  const int count = (attributeIndex_ == 3 && (value & 0x40) == 0) ? 4 : 5;
  attributeIndex_ = u8(attributeIndex_ + 1);
  if (attributeIndex_ >= count) {
    attributeIndex_ = 0;
    portNumber_ = u8((portNumber_ + 1) & 0x7F);
    if (lockstep_) number_ = portNumber_;
  }
}

void Sprites::writePattern(u8 value) {
  pattern_[size_t(patternAddress_ & 0x3FFF)] = value;
  patternAddress_ = u16((patternAddress_ + 1) & 0x3FFF);
}

void Sprites::writeClip(u8 value) {
  clip_[size_t(clipIndex_ & 0x03)] = value;
  clipIndex_ = u8((clipIndex_ + 1) & 0x03);
}

int Sprites::pixel(int sprite, int column, int row, u8 transparency) const {
  const bool four = fourBit(sprite);
  // Rotation happens before mirroring, so a mirrored rotation reads the
  // source cell from the already mirrored destination.
  int cx = mirrorX(sprite) ? Size - 1 - column : column;
  int cy = mirrorY(sprite) ? Size - 1 - row : row;
  if (rotate(sprite)) {
    const int rotatedX = cy;
    cy = Size - 1 - cx;
    cx = rotatedX;
  }

  const int address = pattern(sprite) * PatternStride;
  const u8 packed =
      pattern_[size_t((address + (four ? cy * 8 + cx / 2 : cy * Size + cx)) &
                      0x3FFF)];
  const u8 index = four ? u8((cx & 1) ? (packed & 0x0F) : (packed >> 4)) : packed;
  const u8 hidden = four ? u8(transparency & 0x0F) : transparency;
  return index == hidden ? -1 : int(index);
}

} // namespace mynext
