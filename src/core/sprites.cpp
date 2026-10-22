#include "core/sprites.h"

namespace rainbow {

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
  // Register 0x09 goes back to its default on reset, so the sprite number
  // stops following port 0x303B as well.
  lockstep_ = false;
  collision_ = false;
  overflow_ = false;
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

SpriteState Sprites::anchorState(int sprite) const {
  SpriteState state;
  state.visible = visible(sprite);
  state.x = x(sprite);
  state.y = y(sprite);
  state.pattern = pattern(sprite);
  state.paletteOffset = paletteOffset(sprite);
  state.fourBit = fourBit(sprite);
  state.mirrorX = mirrorX(sprite);
  state.mirrorY = mirrorY(sprite);
  state.rotate = rotate(sprite);
  state.scaleX = scaleX(sprite);
  state.scaleY = scaleY(sprite);
  return state;
}

SpriteState Sprites::relativeState(int sprite, const SpriteState& anchor,
                                   bool unified) const {
  SpriteState state;
  // An invisible anchor hides the whole group.
  state.visible = anchor.visible && visible(sprite);
  state.fourBit = anchor.fourBit;
  state.pattern =
      (pattern(sprite) + ((attribute(sprite, 4) & 0x01) ? anchor.pattern : 0)) &
      0x7F;
  // Bit 0 of byte 2 is the relative palette offset bit on a relative sprite.
  state.paletteOffset = (paletteOffset(sprite) +
                         ((attribute(sprite, 2) & 0x01) ? anchor.paletteOffset : 0)) &
                        0x0F;

  // Bytes 0 and 1 hold signed offsets from the anchor's origin.
  int offsetX = int(i8(attribute(sprite, 0)));
  int offsetY = int(i8(attribute(sprite, 1)));

  if (unified) {
    // A big sprite behaves as one 16x16 image: the offsets scale with the
    // anchor and swing round it, and every part copies the anchor's flags.
    // The rotation turns the offset a quarter clockwise, matching the cell
    // mapping in pixel().
    offsetX *= anchor.scaleX;
    offsetY *= anchor.scaleY;
    if (anchor.rotate) {
      const int turned = offsetX;
      offsetX = -offsetY;
      offsetY = turned;
    }
    if (anchor.mirrorX) offsetX = -offsetX;
    if (anchor.mirrorY) offsetY = -offsetY;
    state.mirrorX = anchor.mirrorX;
    state.mirrorY = anchor.mirrorY;
    state.rotate = anchor.rotate;
    state.scaleX = anchor.scaleX;
    state.scaleY = anchor.scaleY;
  } else {
    state.mirrorX = mirrorX(sprite);
    state.mirrorY = mirrorY(sprite);
    state.rotate = rotate(sprite);
    state.scaleX = scaleX(sprite);
    state.scaleY = scaleY(sprite);
  }

  // The mask keeps a negative offset wrapping round the 512 pixel surface.
  state.x = (anchor.x + offsetX) & 0x1FF;
  state.y = (anchor.y + offsetY) & 0x1FF;
  return state;
}

std::array<SpriteState, Sprites::Count> Sprites::resolve() const {
  std::array<SpriteState, Count> states{};
  // The hardware walks the list upwards and remembers the last anchor, so a
  // relative sprite cannot see anything later in the list.
  SpriteState anchor;
  bool haveAnchor = false;
  bool anchorUnified = false;
  for (int n = 0; n < Count; ++n) {
    if (relative(n)) {
      states[size_t(n)] = relativeState(n, anchor, haveAnchor && anchorUnified);
      continue;
    }
    anchor = anchorState(n);
    anchorUnified = unified(n);
    haveAnchor = true;
    states[size_t(n)] = anchor;
  }
  return states;
}

int Sprites::pixel(const SpriteState& state, int column, int row,
                   u8 transparency) const {
  const bool four = state.fourBit;
  // Rotation happens before mirroring, so a mirrored rotation reads the
  // source cell from the already mirrored destination.
  int cx = state.mirrorX ? Size - 1 - column : column;
  int cy = state.mirrorY ? Size - 1 - row : row;
  if (state.rotate) {
    const int rotatedX = cy;
    cy = Size - 1 - cx;
    cx = rotatedX;
  }

  const int address = state.pattern * PatternStride;
  const u8 packed =
      pattern_[size_t((address + (four ? cy * 8 + cx / 2 : cy * Size + cx)) &
                      0x3FFF)];
  const u8 index = four ? u8((cx & 1) ? (packed & 0x0F) : (packed >> 4)) : packed;
  const u8 hidden = four ? u8(transparency & 0x0F) : transparency;
  return index == hidden ? -1 : int(index);
}

} // namespace rainbow
