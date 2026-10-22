// Golden-frame tests: each scene renders one frame of a known picture and
// compares the whole 320x256 surface against a recorded hash. A rendering
// change that is not intentional fails the test with the scene name, and the
// pixel assertions above each hash pin down what the picture is supposed to
// look like, so a hash cannot quietly record a broken renderer.
//
// The scenes build their own memory contents, so no copyrighted ROM is needed.

#include <string>
#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/palette.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::NextPalette;
using rainbow::u16;
using rainbow::u32;
using rainbow::u64;
using rainbow::u8;

namespace {

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

// FNV-1a over the finished frame, most significant byte first so the value does
// not depend on the host endianness.
u64 frameHash(const Machine& m) {
  const size_t count = size_t(Machine::FrameWidth) * size_t(Machine::FrameHeight);
  const u32* fb = m.framebuffer();
  u64 hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) {
    const u32 pixel = fb[i];
    for (int shift = 24; shift >= 0; shift -= 8) {
      hash ^= u8(pixel >> shift);
      hash *= 0x100000001B3ull;
    }
  }
  return hash;
}

#define CHECK_FRAME_HASH(expected, scene)                                     \
  do {                                                                        \
    const u64 hash_ = frameHash(m);                                            \
    if (hash_ != u64(expected))                                               \
      ::testing::fail(__FILE__, __LINE__,                                     \
                      std::string("golden frame changed for ") + (scene) +    \
                          ": expected " + std::to_string(u64(expected)) +     \
                          ", got " + std::to_string(hash_));                  \
  } while (0)

// A pixel of the 256x192 screen, in screen coordinates.
u32 pixelAt(const Machine& m, int x, int y) {
  const int width = Machine::FrameWidth;
  return m.framebuffer()[size_t(Machine::BorderHeight + y) * size_t(width) +
                         size_t(Machine::BorderWidth + x)];
}

// Sprites, the tilemap and Layer 2 address the whole surface, border included.
u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

// The classic interleaved screen layout.
u16 screenAddress(int x, int y) {
  return u16(0x4000 | ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2) |
             (x >> 3));
}

u16 attrAddress(int x, int y) {
  return u16(0x5800 | ((y >> 3) * 32 + (x >> 3)));
}

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

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

// Bank 5 is the ULA screen; the boot MMU maps its two halves at 0x4000/0x6000.
void mapScreenBank(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
}

void clearScreen(Machine& m) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(0x4000 + i), 0x00, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(0x5800 + i), 0x00, 0);
}

// The recorded frames. Regenerate deliberately, never to make a red test green:
// a change here means the renderer now draws something different.
constexpr u64 Hash48kClassic = 0xB22D339B57F81BA5ull;
constexpr u64 Hash48kFlashOff = 0x30CE77F06E152325ull;
constexpr u64 Hash48kFlashOn = 0x2D10119B3C482325ull;
constexpr u64 HashTimexHires = 0x39809A7ACB2A471Dull;
constexpr u64 HashUlaPlus = 0xBBDAF113D06DF225ull;
constexpr u64 HashLayer2 = 0x289D9DDCA89D09D1ull;
constexpr u64 HashSprites = 0xCA21C1FD60989B25ull;
constexpr u64 HashTilemap = 0x3D65BBD25A402325ull;

} // namespace

// Every cell gets its own ink, paper and brightness, so the picture exercises
// the ink/paper split, the bright bit and the classic interleaved addressing in
// one frame.
TEST(golden_frame_48k_classic_screen) {
  Machine m(MachineType::Spectrum48);

  for (int row = 0; row < 24; ++row) {
    for (int col = 0; col < 32; ++col) {
      const u8 ink = u8(col & 0x07);
      const u8 paper = u8(row & 0x07);
      const u8 bright = ((col ^ row) & 0x08) ? 0x40 : 0x00;
      m.memWrite(screenAddress(col * 8, row * 8),
                 u8(0xA5 ^ (col * 11 + row * 7)), 0);
      m.memWrite(attrAddress(col * 8, row * 8), u8(bright | (paper << 3) | ink), 0);
    }
  }
  m.ioWrite(0x00FE, 0x04, 0); // border 4, green

  renderOneFrame(m);

  CHECK_EQ(m.framebuffer()[0], Machine::Palette[4]);
  // Cell (0,0) is ink 0 on paper 0 with the first pixel lit, which is black.
  CHECK_EQ(pixelAt(m, 0, 0), Machine::Palette[0]);
  // Cell (1,0) is ink 1: 0xA5 ^ 11 = 0xAE, so the first pixel is lit and the
  // second is not.
  CHECK_EQ(pixelAt(m, 8, 0), Machine::Palette[1]);
  CHECK_EQ(pixelAt(m, 9, 0), Machine::Palette[0]);

  CHECK_FRAME_HASH(Hash48kClassic, "48K classic screen");
}

// The flash bit swaps ink and paper on a slow timer. The first frame is the
// un-flashed phase; the scene then waits for the swap rather than assuming a
// particular flash rate.
TEST(golden_frame_48k_flash_blinks) {
  Machine m(MachineType::Spectrum48);

  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(0x4000 + i), 0xFF, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(0x5800 + i), 0xC7, 0);

  renderOneFrame(m);
  const u32 firstPhase = pixelAt(m, 0, 0);
  // Flash, bright, ink 7 on paper 0: the cell alternates between bright white
  // and bright black.
  CHECK(firstPhase == Machine::Palette[15] || firstPhase == Machine::Palette[8]);
  CHECK_FRAME_HASH(Hash48kFlashOff, "48K flash, first phase");

  u32 secondPhase = firstPhase;
  for (int frame = 0; frame < 40 && secondPhase == firstPhase; ++frame) {
    renderOneFrame(m);
    secondPhase = pixelAt(m, 0, 0);
  }
  CHECK(secondPhase != firstPhase);
  CHECK(secondPhase == Machine::Palette[15] || secondPhase == Machine::Palette[8]);
  CHECK_FRAME_HASH(Hash48kFlashOn, "48K flash, second phase");
}

// Hi-res builds each 512-pixel line from the two display files. Column zero is
// kept exactly as the unit test pins it down; the rest of the line carries a
// pattern so the hash covers the interleaving.
TEST(golden_frame_timex_hires) {
  Machine m(MachineType::Next);
  clearScreen(m);

  m.memWrite(0x4000, 0xFF, 0); // first file: even hi-res columns, all ink
  m.memWrite(0x6000, 0x00, 0); // second file: odd columns, all paper
  for (u16 column = 1; column < 32; ++column) {
    m.memWrite(u16(0x4000 + column), u8(0xF0 | (column & 0x0F)), 0);
    m.memWrite(u16(0x6000 + column), u8((column * 7) & 0xFF), 0);
  }

  m.ioWrite(0x00FF, 0x06, 0); // HIRES, paper black on ink white
  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), 0xFFFFFFFFu); // hi-res pixels 0 and 1 both ink
  CHECK_EQ(pixelAt(m, 3, 0), 0xFFFFFFFFu); // pairs 0-3 cover pixels 0-7
  CHECK_EQ(pixelAt(m, 4, 0), 0xFF000000u); // pixels 8-15 come from the second file

  CHECK_FRAME_HASH(HashTimexHires, "Timex hi-res");
}

// ULA+ replaces the sixteen-colour table with three blocks of sixteen, selected
// by bright and flash.
TEST(golden_frame_ulaplus_palette) {
  Machine m(MachineType::Next);
  clearScreen(m);

  for (u8 i = 0; i < 0x10; ++i) {
    m.ioWrite(0xBF3B, i, 0);
    m.ioWrite(0xFF3B, u8(i * 0x11), 0);
    m.ioWrite(0xBF3B, u8(0x10 + i), 0);
    m.ioWrite(0xFF3B, u8(0xF0 | i), 0);
    m.ioWrite(0xBF3B, u8(0x20 + i), 0);
    m.ioWrite(0xFF3B, u8(0x0F ^ i), 0);
  }
  m.writeNextReg(0x68, 0x08); // ULA+ on

  // Bright and flash pick the block, ink and paper pick the entry inside it, so
  // the picture walks through all three blocks of sixteen.
  for (int row = 0; row < 24; ++row) {
    for (int col = 0; col < 32; ++col) {
      const u8 ink = u8(col & 0x07);
      const u8 paper = u8((col >> 3) & 0x07);
      const u8 bright = ((col ^ row) & 0x08) ? 0x40 : 0x00;
      const u8 flash = ((col ^ row) & 0x10) ? 0x80 : 0x00;
      m.memWrite(screenAddress(col * 8, row * 8), 0xFF, 0);
      m.memWrite(attrAddress(col * 8, row * 8),
                 u8(bright | flash | (paper << 3) | ink), 0);
    }
  }

  renderOneFrame(m);

  // Plain block, ink 0, entry 0x00 = 0x00.
  CHECK_EQ(pixelAt(m, 0, 0), Black);
  // Cell 11 is bright ink 3 on paper 1, so entry 0x10 + 3 = 0xF3.
  CHECK_EQ(pixelAt(m, 11 * 8, 0), argb8(0xF3));
  // Cell 22 sets the flash bit and picks ink 6, entry 0x20 + 6 = 0x09.
  CHECK_EQ(pixelAt(m, 22 * 8, 0), argb8(0x09));

  CHECK_FRAME_HASH(HashUlaPlus, "ULA+ palette");
}

// Layer 2 covers the ULA, so a gradient through the palette is the whole
// picture. Only the first 16K of the active bank is reachable from 0xC000 in
// the boot MMU, which is the top 64 rows.
TEST(golden_frame_layer2_gradient) {
  Machine m(MachineType::Next);
  clearScreen(m);

  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 256; ++x) {
      m.memWrite(u16(0xC000 + y * 256 + x), u8(x + y * 3), 0);
    }
  }

  m.writeNextReg(0x12, 0x00); // active bank 0
  m.writeNextReg(0x69, 0x80); // Layer 2 on, above the ULA
  m.writeNextReg(0x43, 0x10); // palette writes land in the Layer 2 first palette
  writePalette8(m, 0x00, 0x00);
  writePalette8(m, 0x01, 0xE0);
  writePalette8(m, 0x02, 0x1C);
  writePalette8(m, 0x03, 0x03);
  m.writeNextReg(0x43, 0x00);

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x00));
  CHECK_EQ(pixelAt(m, 1, 0), argb8(0xE0));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0x1C));
  CHECK_EQ(pixelAt(m, 3, 0), argb8(0x03));

  CHECK_FRAME_HASH(HashLayer2, "Layer 2 gradient");
}

// Two sprites over a black ULA: one at its natural position, one with a palette
// offset, so the hash covers positioning, the pattern fetch and the offset.
TEST(golden_frame_sprites) {
  Machine m(MachineType::Next);
  clearScreen(m);

  m.writeNextReg(0x43, 0x20); // bits 6:4 = 010 -> sprites first palette
  writePalette8(m, 0x01, 0x1C); // bright green
  writePalette8(m, 0x02, 0xE3); // bright magenta
  writePalette8(m, 0x21, 0x1C);
  writePalette8(m, 0x22, 0xE3);
  m.writeNextReg(0x43, 0x00);

  // A pattern that fills its cell with colour 1, except the top row (colour 2)
  // and the bottom row (the transparency index).
  m.ioWrite(0x303B, 0x00, 0);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      u8 value = 0x01;
      if (row == 0) value = 0x02;
      if (row == 15) value = 0xE3;
      m.ioWrite(0x005B, value, 0);
    }
  }

  const auto place = [&m](int sprite, int x, int y, u8 paletteOffset) {
    m.writeNextReg(0x34, u8(sprite));
    m.writeNextReg(0x35, u8(x & 0xFF));
    m.writeNextReg(0x36, u8(y & 0xFF));
    m.writeNextReg(0x37, u8(((x >> 8) & 0x01) | paletteOffset));
    m.writeNextReg(0x38, 0x80); // visible, pattern 0
    m.writeNextReg(0x39, 0x00);
  };
  place(0, 32, 32, 0x00);
  place(1, 96, 64, 0x20);

  m.writeNextReg(0x15, 0x01); // sprites on
  renderOneFrame(m);

  CHECK_EQ(surfacePixel(m, 32, 32), Magenta);
  CHECK_EQ(surfacePixel(m, 32, 33), Green);
  CHECK_EQ(surfacePixel(m, 32, 47), Black); // transparent row
  CHECK_EQ(surfacePixel(m, 96, 65), Green);

  CHECK_FRAME_HASH(HashSprites, "sprites");
}

// The tilemap fills the whole surface, border included, from tile definitions in
// bank 5.
TEST(golden_frame_tilemap) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
  m.writeNextReg(0x6E, 0x30); // map at bank offset 0x3000
  m.writeNextReg(0x6F, 0x20); // definitions at bank offset 0x2000
  m.writeNextReg(0x68, 0x80); // ULA out of the way

  for (u16 i = 0; i < 0x0800; ++i) m.memWrite(u16(0x7000 + i), 0x00, 0);
  for (u16 i = 0; i < 0x1000; ++i) m.memWrite(u16(0x6000 + i), 0x00, 0);

  // Tile 1 is a solid block of colour 1, tile 2 a checkerboard of 1 and 2.
  for (int i = 0; i < 32; ++i) m.memWrite(u16(0x6000 + 1 * 32 + i), 0x11, 0);
  for (int i = 0; i < 32; ++i) m.memWrite(u16(0x6000 + 2 * 32 + i), 0x12, 0);

  for (int row = 0; row < 32; ++row) {
    for (int column = 0; column < 40; ++column) {
      const u16 address = u16(0x7000 + (row * 40 + column) * 2);
      m.memWrite(address, u8((column / 8) % 2 ? 2 : 1), 0);
      m.memWrite(u16(address + 1), u8((row / 8) % 2 ? 0x01 : 0x00), 0);
    }
  }

  m.writeNextReg(0x43, 0x30); // tilemap first palette
  writePalette8(m, 0x01, 0x1C); // bright green
  writePalette8(m, 0x02, 0xE3); // bright magenta
  m.writeNextReg(0x43, 0x00);

  m.writeNextReg(0x6B, 0x80); // tilemap on
  renderOneFrame(m);

  CHECK_EQ(surfacePixel(m, 10, 10), Green);
  CHECK_EQ(surfacePixel(m, 0, 0), Green);

  CHECK_FRAME_HASH(HashTilemap, "tilemap");
}
