#pragma once

#include <array>

#include "core/types.h"

namespace rainbow {

// The Next's colour hardware: eight palettes of 256 entries, each entry a 9-bit
// RGB333 colour. Register 0x43 picks the palette being written, 0x40 the entry
// and 0x41/0x44 the colour; writes auto-increment the entry index so a program
// can stream a whole palette through two ports.
class NextPalette {
public:
  static constexpr int Entries = 256;
  // ULA, Layer 2, sprites and the tilemap each have two palettes.
  static constexpr int Palettes = 8;

  static constexpr int Ula0 = 0;
  static constexpr int Ula1 = 1;
  static constexpr int Layer2First = 2;
  static constexpr int Layer2Second = 3;
  static constexpr int SpritesFirst = 4;
  static constexpr int SpritesSecond = 5;
  static constexpr int TilemapFirst = 6;
  static constexpr int TilemapSecond = 7;

  NextPalette() { reset(); }

  void reset();

  // Register 0x43.
  u8 control() const { return control_; }
  void setControl(u8 value);
  // The palette a write lands in. Bits 6:4 of the control byte hold the layer
  // (ULA, Layer 2, sprites, tilemap) in the low two bits and the first or
  // second palette of that layer in bit 2, which is the reverse of the order
  // the palette indices are numbered in.
  int selected() const {
    const int code = (control_ >> 4) & 0x07;
    return ((code & 0x03) << 1) | ((code >> 2) & 0x01);
  }
  bool autoIncrement() const { return (control_ & 0x80) == 0; }

  // Register 0x40.
  u8 index() const { return index_; }
  void setIndex(u8 value) { index_ = value; highBytePending_ = true; }

  // Register 0x41: an 8-bit RRRGGGBB colour.
  void write8(u8 value);
  // ULA+ writes an 8-bit RRRGGGBB colour into an arbitrary entry without
  // touching the register index or the auto-increment state.
  void write8(int palette, u8 index, u8 value);
  u8 read8(int palette, u8 index) const;
  // Register 0x44: two writes, RRRGGGBB then the ninth bit.
  void write9(u8 value);
  // The value register 0x41 reads back.
  u8 lastValue() const { return last_; }

  u16 colour9(int palette, u8 index) const { return colours_[size_t(palette & 7)][index]; }
  void setColour9(int palette, u8 index, u16 rgb9);

  // ARGB8888, ready for the framebuffer.
  u32 colour(int palette, u8 index) const {
    return argb_[size_t(palette & 7)][index];
  }

  static u32 toArgb(u16 rgb9);

private:
  void store(int palette, u8 index, u16 rgb9);

  std::array<std::array<u16, Entries>, Palettes> colours_{};
  std::array<std::array<u32, Entries>, Palettes> argb_{};
  u8 control_ = 0;
  u8 index_ = 0;
  u8 last_ = 0;
  u8 pendingHigh_ = 0;
  bool highBytePending_ = true;
};

} // namespace rainbow
