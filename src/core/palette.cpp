#include "core/palette.h"

namespace rainbow {
namespace {

// RRR GGG BBB, three bits per channel.
constexpr u16 rgb9(u16 r, u16 g, u16 b) { return u16((r << 6) | (g << 3) | b); }

// The sixteen classic Spectrum colours, so that a Next with an untouched
// palette looks exactly like the machine it is emulating.
constexpr u16 ClassicColours[16] = {
    rgb9(0, 0, 0), rgb9(0, 0, 6), rgb9(6, 0, 0), rgb9(6, 0, 6),
    rgb9(0, 6, 0), rgb9(0, 6, 6), rgb9(6, 6, 0), rgb9(6, 6, 6),
    rgb9(0, 0, 0), rgb9(0, 0, 7), rgb9(7, 0, 0), rgb9(7, 0, 7),
    rgb9(0, 7, 0), rgb9(0, 7, 7), rgb9(7, 7, 0), rgb9(7, 7, 7),
};

} // namespace

u32 NextPalette::toArgb(u16 value) {
  const u32 r = (value >> 6) & 0x07;
  const u32 g = (value >> 3) & 0x07;
  const u32 b = value & 0x07;
  return 0xFF000000u | ((r * 255u / 7u) << 16) | ((g * 255u / 7u) << 8) |
         (b * 255u / 7u);
}

void NextPalette::reset() {
  control_ = 0;
  index_ = 0;
  last_ = 0;
  highBytePending_ = true;

  for (auto& palette : colours_) palette.fill(0);

  // The ULA palette repeats the classic colours in its ink half (0-15) and its
  // paper half (16-31); everything else starts black.
  for (int entry = 0; entry < 16; ++entry) {
    colours_[Ula0][size_t(entry)] = ClassicColours[entry];
    colours_[Ula0][size_t(entry + 16)] = ClassicColours[entry];
    colours_[Ula1][size_t(entry)] = ClassicColours[entry];
    colours_[Ula1][size_t(entry + 16)] = ClassicColours[entry];
  }

  // The graphics palettes default to a plain 3-3-2 ramp, which makes Layer 2
  // output visible instead of uniformly black.
  for (int entry = 0; entry < Entries; ++entry) {
    const u16 r = u16((entry >> 5) & 0x07);
    const u16 g = u16((entry >> 2) & 0x07);
    const u16 b2 = u16(entry & 0x03);
    const u16 b = u16((b2 << 1) | (b2 != 0 ? 1 : 0));
    const u16 value = rgb9(r, g, b);
    for (int palette : {Layer2First, Layer2Second, SpritesFirst, SpritesSecond,
                        TilemapFirst, TilemapSecond}) {
      colours_[size_t(palette)][size_t(entry)] = value;
    }
  }

  for (int palette = 0; palette < Palettes; ++palette) {
    for (int entry = 0; entry < Entries; ++entry) {
      argb_[size_t(palette)][size_t(entry)] =
          toArgb(colours_[size_t(palette)][size_t(entry)]);
    }
  }
}

void NextPalette::setControl(u8 value) {
  control_ = value;
  highBytePending_ = true;
}

void NextPalette::store(int palette, u8 index, u16 value) {
  colours_[size_t(palette)][index] = value;
  argb_[size_t(palette)][index] = toArgb(value);
}

void NextPalette::setColour9(int palette, u8 index, u16 value) {
  store(palette, index, u16(value & 0x01FF));
}

void NextPalette::write8(u8 value) {
  // RRRGGGBB; the ninth bit is the OR of the two blue bits, so a fully lit
  // blue channel reaches 111.
  const u16 b2 = u16(value & 0x03);
  const u16 rgb = u16(((value & 0xE0u) << 1) | (((value & 0x1Cu) >> 2) << 3) |
                      ((b2 << 1) | (b2 != 0 ? 1 : 0)));
  store(selected(), index_, rgb);
  last_ = value;
  highBytePending_ = true;
  if (autoIncrement()) ++index_;
}

void NextPalette::write8(int palette, u8 index, u8 value) {
  const u16 b2 = u16(value & 0x03);
  store(palette, index,
        u16(((value & 0xE0u) << 1) | (((value & 0x1Cu) >> 2) << 3) |
            ((b2 << 1) | (b2 != 0 ? 1 : 0))));
}

u8 NextPalette::read8(int palette, u8 index) const {
  const u16 rgb = colours_[size_t(palette & 7)][index];
  return u8((((rgb >> 6) & 0x07) << 5) | (((rgb >> 3) & 0x07) << 2) |
            ((rgb & 0x07) >> 1));
}

void NextPalette::write9(u8 value) {
  if (highBytePending_) {
    pendingHigh_ = value;
    highBytePending_ = false;
    return;
  }
  const u16 b2 = u16(pendingHigh_ & 0x03);
  const u16 rgb = u16(((pendingHigh_ & 0xE0u) << 1) |
                      (((pendingHigh_ & 0x1Cu) >> 2) << 3) |
                      ((b2 << 1) | (value & 0x01)));
  store(selected(), index_, rgb);
  last_ = pendingHigh_;
  highBytePending_ = true;
  if (autoIncrement()) ++index_;
}

} // namespace rainbow
