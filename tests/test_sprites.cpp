#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/sprites.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::NextPalette;
using mynext::Sprites;
using mynext::u16;
using mynext::u32;
using mynext::u8;

namespace {

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

// Sprites are positioned on the full 320x256 surface, which is exactly the
// framebuffer, so a sprite at (32,32) lands on the top left ULA pixel.
u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

// The default palette entries are a plain ramp; every test paints the entry it
// looks for so the expected colour is obvious.
void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

void selectSpritePalette(Machine& m) {
  m.writeNextReg(0x43, 0x20); // bits 6:4 = 010 -> sprites first palette
  writePalette8(m, 0x01, 0x1C); // bright green
  writePalette8(m, 0x02, 0xE3); // bright magenta
  writePalette8(m, 0x21, 0x1C); // bright green with palette offset 2
  writePalette8(m, 0x22, 0xE3); // bright magenta with palette offset 2
  writePalette8(m, 0x25, 0x03); // dark blue, for the palette offset test
  m.writeNextReg(0x43, 0x00);
}

// A sprite that fills its whole 16x16 cell with pattern colour 1, except the
// top row, which uses colour 2, and the bottom row, which is transparent.
void writeFullPattern(Machine& m, int pattern) {
  m.ioWrite(0x303B, u8(pattern), 0);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      u8 value = 0x01;
      if (row == 0) value = 0x02;
      if (row == 15) value = 0xE3;
      m.ioWrite(0x005B, value, 0);
    }
  }
}

void writeAnchorSprite(Machine& m, int sprite, int x, int y, int pattern) {
  m.writeNextReg(0x34, u8(sprite));
  m.writeNextReg(0x35, u8(x & 0xFF));
  m.writeNextReg(0x36, u8(y & 0xFF));
  m.writeNextReg(0x37, u8((x >> 8) & 0x01));
  m.writeNextReg(0x38, u8(0x80 | (pattern & 0x3F)));
  m.writeNextReg(0x39, u8((pattern >> 6) & 0x01));
}

// Extended sprites need the extra attribute byte, which carries the scale.
void writeExtendedSprite(Machine& m, int sprite, int x, int y, int pattern,
                         u8 scaleX, u8 scaleY) {
  m.writeNextReg(0x34, u8(sprite));
  m.writeNextReg(0x35, u8(x & 0xFF));
  m.writeNextReg(0x36, u8(y & 0xFF));
  m.writeNextReg(0x37, u8((x >> 8) & 0x01));
  m.writeNextReg(0x38, u8(0xC0 | (pattern & 0x3F)));
  m.writeNextReg(0x39, u8(((pattern >> 6) & 0x01) | (scaleX << 3) |
                          (scaleY << 1) | ((y >> 8) & 0x01)));
}

void enableSprites(Machine& m, u8 control = 0x01) {
  m.writeNextReg(0x15, control);
}

} // namespace

TEST(sprites_are_hidden_until_register_15_enables_them) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF000000u); // black, the ULA paper

  enableSprites(m);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF00FF00u); // bright green
}

TEST(sprite_attributes_are_stored_per_sprite) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x34, 7);
  m.writeNextReg(0x35, 0x34);
  m.writeNextReg(0x36, 0x56);
  m.writeNextReg(0x37, 0x01);
  m.writeNextReg(0x38, 0x80 | 5);
  m.writeNextReg(0x39, 0x00);

  CHECK_EQ(m.sprites().attribute(7, 0), 0x34);
  CHECK_EQ(m.sprites().attribute(7, 1), 0x56);
  CHECK(m.sprites().visible(7));
  CHECK_EQ(m.sprites().x(7), 0x134);
  CHECK_EQ(m.sprites().y(7), 0x56);
  // N5..N0 = 000101 with N6 = 0 reads back as pattern 10.
  CHECK_EQ(m.sprites().pattern(7), 10);
}

TEST(sprite_pattern_number_puts_n6_last) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x34, 0);
  m.writeNextReg(0x38, 0xC0 | 0x05); // extended, N5..N0 = 000101
  m.writeNextReg(0x39, 0xC0);        // H = 1, N6 = 1

  CHECK_EQ(m.sprites().pattern(0), 11);
  CHECK_EQ(m.sprites().fourBit(0), 1); // bit 7 of attribute 4 selects 4-bit
}

TEST(sprite_uses_the_palette_offset) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  m.writeNextReg(0x34, 0);
  m.writeNextReg(0x37, 0x20); // palette offset 2 shifts colours by 0x20
  enableSprites(m);

  renderOneFrame(m);
  // Colour 2 on the top row lands on entry 0x22, colour 1 on entry 0x21.
  CHECK_EQ(surfacePixel(m, 32, 32), 0xFFFF00FFu);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF00FF00u);
}

TEST(sprite_transparent_index_is_skipped) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  enableSprites(m);

  renderOneFrame(m);
  // Row 15 of the pattern is the transparency index, so the ULA shows through.
  CHECK_EQ(surfacePixel(m, 32, 32 + 15), 0xFF000000u);
}

TEST(sprite_transparency_register_can_be_changed) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  enableSprites(m);
  m.writeNextReg(0x4B, 0x01); // colour 1 is now transparent

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF000000u); // was bright green
  CHECK_EQ(surfacePixel(m, 32, 32), 0xFFFF00FFu); // colour 2 still drawn
}

TEST(four_bit_sprites_pack_two_pixels_per_byte) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  // 4-bit pattern: one row is eight bytes, the left pixel in the high nibble.
  m.ioWrite(0x303B, 0, 0);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 8; ++column) {
      m.ioWrite(0x005B, 0x12, 0);
    }
  }
  writeExtendedSprite(m, 0, 32, 32, 0, 0, 0);
  m.writeNextReg(0x39, 0x80); // H = 1 -> 4-bit pattern
  enableSprites(m);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 32), 0xFF00FF00u); // high nibble, colour 1
  CHECK_EQ(surfacePixel(m, 33, 32), 0xFFFF00FFu); // low nibble, colour 2
}

TEST(sprite_mirrors_horizontally) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  m.writeNextReg(0x37, 0x08); // X mirror
  enableSprites(m);

  renderOneFrame(m);
  // The top row is colour 2 before mirroring and stays on the top row.
  CHECK_EQ(surfacePixel(m, 32, 32), 0xFFFF00FFu);
  CHECK_EQ(surfacePixel(m, 47, 32), 0xFFFF00FFu);
  CHECK_EQ(surfacePixel(m, 32, 47), 0xFF000000u);
}

TEST(sprite_rotates_clockwise) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  // Only the left column of the pattern is drawn, so a rotation has to move it
  // to the top row.
  m.ioWrite(0x303B, 0, 0);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      m.ioWrite(0x005B, u8(column == 0 ? 0x01 : 0x00), 0);
    }
  }
  m.writeNextReg(0x4B, 0x00); // colour 0 is transparent
  writeAnchorSprite(m, 0, 32, 32, 0);
  m.writeNextReg(0x37, 0x02); // rotate
  enableSprites(m);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 32), 0xFF00FF00u);  // left column is now the top row
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF000000u);
}

TEST(sprite_scales_with_the_extended_attribute) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeExtendedSprite(m, 0, 32, 32, 0, 1, 1); // 2x in both directions
  enableSprites(m);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 34), 0xFF00FF00u);
  CHECK_EQ(surfacePixel(m, 33, 35), 0xFF00FF00u); // same cell, doubled
  // 32 pixels tall: row 15 of the pattern covers the last two rows.
  CHECK_EQ(surfacePixel(m, 32, 32 + 30), 0xFF000000u);
  CHECK_EQ(surfacePixel(m, 32, 32 + 31), 0xFF000000u);
}

TEST(sprite_stays_inside_the_screen_without_over_border) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 24, 40, 0); // eight pixels into the left border
  enableSprites(m);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 24, 41), 0xFF000000u); // border, not the sprite
  CHECK_EQ(surfacePixel(m, 32, 41), 0xFF00FF00u); // first visible pixel

  enableSprites(m, 0x03); // bit 1: sprites over the border
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 24, 41), 0xFF00FF00u);
}

TEST(sprite_priority_bit_puts_sprite_zero_on_top) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  // Sprite 0 and sprite 1 overlap; the pattern's colour comes from the palette
  // offset, so they are told apart by colour rather than by position.
  writeAnchorSprite(m, 0, 32, 32, 0);
  m.writeNextReg(0x34, 0);
  m.writeNextReg(0x37, 0x20); // sprite 0 draws with entry 0x22
  writeAnchorSprite(m, 1, 32, 32, 0);
  m.writeNextReg(0x34, 1);
  m.writeNextReg(0x37, 0x30); // sprite 1 draws with entry 0x31

  // Give the two palette offsets distinct colours.
  m.writeNextReg(0x43, 0x20);
  writePalette8(m, 0x21, 0x1C); // bright green, sprite 0
  writePalette8(m, 0x31, 0xE3); // bright magenta, sprite 1
  m.writeNextReg(0x43, 0x00);

  enableSprites(m, 0x01); // sprite 127 on top, so sprite 1 wins
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFFFF00FFu);

  enableSprites(m, 0x41); // bit 6: sprite 0 on top
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF00FF00u);
}

TEST(sprites_stay_behind_the_ula_when_the_priority_says_so) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  m.writeNextReg(0x54, 10); // map the screen bank so the ULA has pixels
  m.writeNextReg(0x55, 11);

  enableSprites(m, u8(0x01 | (4 << 2))); // U S L: the ULA is on top
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF000000u);

  enableSprites(m, u8(0x01 | (0 << 2))); // S L U: sprites on top again
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF00FF00u);
}

TEST(sprite_clip_window_hides_pixels) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  writeFullPattern(m, 0);
  writeAnchorSprite(m, 0, 32, 32, 0);
  enableSprites(m);

  m.writeNextReg(0x19, 0);
  m.writeNextReg(0x19, 15);
  m.writeNextReg(0x19, 0);
  m.writeNextReg(0x19, 15);

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 33), 0xFF00FF00u);  // inside the window
  CHECK_EQ(surfacePixel(m, 47, 33), 0xFF00FF00u);
  CHECK_EQ(surfacePixel(m, 48, 33), 0xFF000000u);  // outside
  CHECK_EQ(surfacePixel(m, 32, 48), 0xFF000000u);
}

TEST(sprite_clip_index_resets_through_register_1c) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x19, 4); // X1, then the index is left mid-sequence
  m.writeNextReg(0x1C, 0x02);
  m.writeNextReg(0x19, 7);

  CHECK_EQ(m.sprites().clip()[0], 7); // the first write again
  CHECK_EQ(m.sprites().clip()[1], 255);
}

TEST(sprite_attribute_port_streams_four_bytes_per_sprite) {
  Machine m(MachineType::Next);
  m.ioWrite(0x303B, 3, 0);
  m.ioWrite(0x0057, 0x20, 0); // sprite 3, X
  m.ioWrite(0x0057, 0x30, 0); // Y
  m.ioWrite(0x0057, 0x00, 0);
  m.ioWrite(0x0057, 0x80, 0); // visible, not extended -> the stream restarts here

  CHECK_EQ(m.sprites().attribute(3, 0), 0x20);
  CHECK_EQ(m.sprites().attribute(3, 1), 0x30);
  CHECK_EQ(m.sprites().visible(3), 1);

  m.ioWrite(0x0057, 0x40, 0); // sprite 4, X
  CHECK_EQ(m.sprites().attribute(4, 0), 0x40);
}

TEST(sprite_attribute_port_takes_five_bytes_for_extended_sprites) {
  Machine m(MachineType::Next);
  m.ioWrite(0x303B, 0, 0);
  m.ioWrite(0x0057, 0x00, 0);
  m.ioWrite(0x0057, 0x00, 0);
  m.ioWrite(0x0057, 0x00, 0);
  m.ioWrite(0x0057, 0xC0, 0); // visible and extended
  m.ioWrite(0x0057, 0x02, 0); // attribute 4 belongs to the same sprite
  m.ioWrite(0x0057, 0x99, 0); // sprite 1, X

  CHECK_EQ(m.sprites().attribute(0, 4), 0x02);
  CHECK_EQ(m.sprites().attribute(1, 0), 0x99);
}

TEST(sprite_number_lockstep_follows_port_303b) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x09, 0x10); // bit 4: the two sprite numbers move together
  m.writeNextReg(0x34, 5);
  CHECK_EQ(m.sprites().portNumber(), 5);

  m.ioWrite(0x303B, 9, 0);
  CHECK_EQ(m.sprites().number(), 9);
}

TEST(sprite_reset_clears_the_attributes) {
  Machine m(MachineType::Next);
  writeAnchorSprite(m, 0, 32, 32, 0);
  CHECK(m.sprites().visible(0));

  m.reset();
  CHECK(!m.sprites().visible(0));
  CHECK_EQ(m.sprites().clip()[1], 255);
}
