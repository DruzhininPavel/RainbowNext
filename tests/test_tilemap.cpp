#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/palette.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::NextPalette;
using mynext::u16;
using mynext::u32;
using mynext::u8;

namespace {

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

// The tilemap covers the whole 320x256 surface, border included, so the tests
// address it in the same coordinates the sprites use.
u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

// Bank 5 is mapped at 0x4000 by the boot MMU setup, so the tilemap and the
// tile definitions live in the part of it the ULA screen does not use. The
// definitions take bank offset 0x2000 and the map 0x3000, which keeps both
// clear of the 6912 byte screen at offset 0.
constexpr u16 DefBase = 0x6000;
constexpr u16 MapBase = 0x7000;
constexpr u8 DefHigh = 0x20;
constexpr u8 MapHigh = 0x30;

// An 8-bit palette value is a RRRGGGBB colour, expanded the way the hardware
// does before it reaches the framebuffer.
u32 argb8(u8 value) {
  const u16 b2 = u16(value & 0x03);
  const u16 rgb = u16(((value & 0xE0u) << 1) | (((value & 0x1Cu) >> 2) << 3) |
                      ((b2 << 1) | (b2 != 0 ? 1 : 0)));
  return NextPalette::toArgb(rgb);
}

const u32 Black = argb8(0x00);
const u32 Green = argb8(0x1C);
const u32 Magenta = argb8(0xE3);
const u32 Blue = argb8(0x03);
const u32 Cyan = argb8(0x1B);
const u32 White = argb8(0xFF);

void clearMap(Machine& m) {
  for (u16 i = 0; i < 0x0800; ++i) m.memWrite(u16(MapBase + i), 0x00, 0);
}

void clearDefs(Machine& m) {
  for (u16 i = 0; i < 0x1000; ++i) m.memWrite(u16(DefBase + i), 0x00, 0);
}

void writeEntry(Machine& m, int column, int row, u8 tile, u8 attribute) {
  const u16 address = u16(MapBase + (row * 40 + column) * 2);
  m.memWrite(address, tile, 0);
  m.memWrite(u16(address + 1), attribute, 0);
}

void fillMap(Machine& m, u8 tile, u8 attribute) {
  for (int row = 0; row < 32; ++row) {
    for (int column = 0; column < 40; ++column) {
      writeEntry(m, column, row, tile, attribute);
    }
  }
}

// A tile definition is 32 bytes: four bytes per row, two pixels per byte.
void fillTile(Machine& m, int tile, u8 value) {
  for (int i = 0; i < 32; ++i) {
    m.memWrite(u16(DefBase + tile * 32 + i), value, 0);
  }
}

void writeTileByte(Machine& m, int tile, int row, int column, u8 value) {
  m.memWrite(u16(DefBase + tile * 32 + row * 4 + column), value, 0);
}

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

// 0x43 bits 6:4 pick the palette the write registers land in; 011 is the
// tilemap's first palette and 111 its second.
void writeTilemapPalette(Machine& m, int palette, u8 index, u8 value) {
  m.writeNextReg(0x43, u8(palette == NextPalette::TilemapSecond ? 0x70 : 0x30));
  writePalette8(m, index, value);
  m.writeNextReg(0x43, 0x00);
}

void enableTilemap(Machine& m, u8 control = 0x80) { m.writeNextReg(0x6B, control); }

void disableUla(Machine& m) { m.writeNextReg(0x68, 0x80); }
void enableUla(Machine& m) { m.writeNextReg(0x68, 0x00); }

// A ULA screen of solid ink 0, so any ULA pixel is black and easy to tell
// apart from the tilemap.
void fillUlaWithInk(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(0x4000 + i), 0xFF, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(0x5800 + i), 0x00, 0);
}

void writeClip(Machine& m, u8 x1, u8 x2, u8 y1, u8 y2) {
  m.writeNextReg(0x1B, x1);
  m.writeNextReg(0x1B, x2);
  m.writeNextReg(0x1B, y1);
  m.writeNextReg(0x1B, y2);
}

// A single green tile across the whole map, with the ULA out of the way.
void greenTilemap(Machine& m, u8 control = 0x80) {
  disableUla(m);
  m.writeNextReg(0x6E, MapHigh);
  m.writeNextReg(0x6F, DefHigh);
  clearMap(m);
  clearDefs(m);
  fillMap(m, 1, 0x00);
  fillTile(m, 1, 0x11);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x01, 0x1C);
  enableTilemap(m, control);
}

} // namespace

TEST(tilemap_is_hidden_until_register_6b_enables_it) {
  Machine m(MachineType::Next);
  greenTilemap(m, 0x00); // written but not enabled
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Black);

  enableTilemap(m);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Green);
}

TEST(tilemap_draws_four_bit_pixels_from_the_definition) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  clearDefs(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0xE3); // magenta
  writeTileByte(m, 1, 0, 0, 0x12); // first pixel 1, second pixel 2
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Green);
  CHECK_EQ(surfacePixel(m, 1, 0), Magenta);
  CHECK_EQ(surfacePixel(m, 2, 0), Black); // the rest of the row stays 0
}

TEST(tilemap_palette_offset_shifts_the_colour) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x21, 0xE3);
  writeEntry(m, 0, 0, 1, 0x20); // palette offset 2
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
  CHECK_EQ(surfacePixel(m, 8, 0), Green); // the next tile has no offset
}

TEST(tilemap_transparency_index_is_skipped) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  fillTile(m, 1, 0xFF); // both nibbles are the soft reset transparency index
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x0F, 0x1C);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Black);

  m.writeNextReg(0x4C, 0x01);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Green);
}

TEST(tilemap_scrolls_horizontally) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0xE3);
  fillTile(m, 2, 0x22);
  writeEntry(m, 1, 0, 2, 0x00);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 8, 0), Magenta);

  m.writeNextReg(0x30, 8);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
  CHECK_EQ(surfacePixel(m, 8, 0), Green);
}

TEST(tilemap_scrolls_vertically) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0xE3);
  fillTile(m, 2, 0x22);
  writeEntry(m, 0, 1, 2, 0x00);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 8), Magenta);

  m.writeNextReg(0x31, 8);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
  CHECK_EQ(surfacePixel(m, 0, 8), Green);
}

TEST(tilemap_mirrors_tiles_horizontally) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  clearDefs(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0xE3);
  writeTileByte(m, 1, 0, 3, 0x21); // pixels 6 and 7 are 2 and 1
  writeEntry(m, 0, 0, 1, 0x08);    // mirror X
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Green);   // column 7 of the definition
  CHECK_EQ(surfacePixel(m, 1, 0), Magenta); // column 6
  CHECK_EQ(surfacePixel(m, 6, 0), Black);   // column 1 is empty
  CHECK_EQ(surfacePixel(m, 7, 0), Black);   // and so is column 0
}

TEST(tilemap_rotates_tiles) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  clearDefs(m);
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x03, 0x03); // blue
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x04, 0x1B); // cyan
  writeTileByte(m, 1, 7, 0, 0x33); // bottom row, first pixel 3
  writeTileByte(m, 1, 6, 0, 0x44); // second from bottom, first pixel 4
  writeEntry(m, 0, 0, 1, 0x02);    // rotate
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Blue); // bottom left becomes top left
  CHECK_EQ(surfacePixel(m, 1, 0), Cyan);
}

TEST(tilemap_uses_the_second_palette_when_selected) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  writeTilemapPalette(m, NextPalette::TilemapSecond, 0x01, 0xE3);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Green);

  enableTilemap(m, 0x90); // bit 4 selects the second palette
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 10, 10), Magenta);
}

TEST(tilemap_takes_the_ninth_tile_number_bit_in_512_tile_mode) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  // Tile 300 needs 9600 bytes, so it wraps inside the 4K definition window and
  // lands at bank offset 0x580.
  for (int i = 0; i < 32; ++i) {
    m.memWrite(u16(0x4580 + i), 0x22, 0);
  }
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0xE3);
  writeEntry(m, 0, 0, 44, 0x01); // tile 44 plus bit 8
  enableTilemap(m, 0x82);        // 512 tile mode

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
}

TEST(tilemap_uses_the_default_attribute_without_attributes) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  clearMap(m);
  for (int i = 0; i < 0x0800; ++i) {
    m.memWrite(u16(MapBase + i), 0x01, 0); // one byte per entry, tile 1
  }
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x21, 0xE3);
  m.writeNextReg(0x6C, 0x20); // default attribute: palette offset 2
  enableTilemap(m, 0xA0);     // bit 5 removes the attribute bytes
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
}

TEST(tilemap_clip_window_hides_pixels) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  // X is stored in pairs of pixels, so 2..3 covers x 4..7.
  writeClip(m, 2, 3, 0, 255);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 5, 10), Green);
  CHECK_EQ(surfacePixel(m, 2, 10), Black);
  CHECK_EQ(surfacePixel(m, 8, 10), Black);
}

TEST(tilemap_clip_index_resets_through_register_1c) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  m.writeNextReg(0x1B, 2); // X1 only, leaving the index mid-way
  m.writeNextReg(0x1C, 0x08);
  writeClip(m, 2, 3, 0, 255);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 5, 10), Green);
  CHECK_EQ(surfacePixel(m, 8, 10), Black);
}

TEST(tilemap_reaches_into_the_border) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 3, 3), Green);
  CHECK_EQ(surfacePixel(m, Machine::FrameWidth - 3, 3), Green);
}

TEST(tilemap_sits_over_the_ula_unless_the_tile_says_otherwise) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  fillUlaWithInk(m);
  enableUla(m);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Green); // the tile is on top of the ULA

  fillMap(m, 1, 0x01); // "ULA over tilemap" in the tile attribute
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Black);
}

TEST(tilemap_can_be_forced_on_top_of_the_ula) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  fillUlaWithInk(m);
  enableUla(m);
  fillMap(m, 1, 0x01);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Black);

  enableTilemap(m, 0x81); // bit 0 forces the tilemap over the ULA
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Green);
}

TEST(ula_output_can_be_disabled_through_register_68) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  fillUlaWithInk(m);
  enableUla(m);
  fillMap(m, 1, 0x01); // the tile is under the ULA
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Black);

  disableUla(m);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 40, 40), Green);
}

TEST(tilemap_text_mode_uses_palette_pairs) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  clearMap(m);
  clearDefs(m);
  writeEntry(m, 0, 0, 1, 0x02);          // palette offset 1 -> pair 2/3
  m.memWrite(u16(DefBase + 8), 0x80, 0); // tile 1, row 0: only the ink pixel
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x02, 0x03); // blue paper
  writeTilemapPalette(m, NextPalette::TilemapFirst, 0x03, 0xE3); // magenta ink
  enableTilemap(m, 0x88); // bit 3 selects text mode
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, 0), Magenta);
  CHECK_EQ(surfacePixel(m, 1, 0), Blue);
}

TEST(layer2_covers_the_tilemap_when_the_priority_says_so) {
  Machine m(MachineType::Next);
  greenTilemap(m);
  m.writeNextReg(0x43, 0x10); // bits 6:4 = 001 -> Layer 2 first palette
  writePalette8(m, 0x01, 0xFF); // white
  m.writeNextReg(0x43, 0x00);
  m.memWrite(0xC000, 0x01, 0); // Layer 2 pixel (0, 0), bank 0
  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80); // enable Layer 2

  // Layer 2 starts at surface (32, 32), so both layers meet there.
  m.writeNextReg(0x15, 0x0C); // bits 4:2 = 011 -> L U S
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 32), White);

  m.writeNextReg(0x15, 0x14); // bits 4:2 = 101 -> U L S
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 32), Green);
}
