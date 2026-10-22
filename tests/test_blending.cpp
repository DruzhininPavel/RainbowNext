#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/palette.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::NextPalette;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;

namespace {

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

u32 pixelAt(const Machine& m, int x, int y) {
  const int width = Machine::FrameWidth;
  return m.framebuffer()[size_t(Machine::BorderHeight + y) * size_t(width) +
                         size_t(Machine::BorderWidth + x)];
}

u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

constexpr u16 ScreenBase = 0x4000;

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

const u32 Red = 0xFFFF0000u;    // RRRGGGBB 0xE0
const u32 Green = 0xFF00FF00u;  // RRRGGGBB 0x1C
const u32 Yellow = 0xFFFFFF00u; // red plus green
const u32 DarkGrey = 0xFF484800u;
const u32 Blue = 0xFF0000FFu;   // RRRGGGBB 0x03

// The ULA draws red over the whole screen and Layer 2 holds a single green
// pixel in the top left corner, which the blend modes then mix.
void setupLayers(Machine& m, u8 l2Pixel) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(ScreenBase + i), 0xFF, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(ScreenBase + 0x1800 + i), 0x00, 0);

  m.writeNextReg(0x43, 0x00); // ULA first palette
  writePalette8(m, 0x00, 0xE0);
  m.writeNextReg(0x43, 0x10); // Layer 2 first palette
  writePalette8(m, 0x05, 0x1C);
  writePalette8(m, 0x85, 0x00);

  // Layer 2 lives in bank 0, which the boot MMU maps at 0xC000.
  m.writeNextReg(0x12, 0x00);
  m.memWrite(0xC000, l2Pixel, 0);
  m.writeNextReg(0x69, 0x80);
}

void setPriority(Machine& m, int priority, bool sprites) {
  m.writeNextReg(0x15, u8((priority << 2) | (sprites ? 0x01 : 0x00)));
}

} // namespace

TEST(blending_mode_6_adds_layer2_to_the_ula) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);
  setPriority(m, 6, false);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Yellow);
  // Pixels outside Layer 2 keep the plain ULA colour.
  CHECK_EQ(pixelAt(m, 1, 0), Red);
}

TEST(blending_mode_7_subtracts_five_from_each_channel) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);
  setPriority(m, 7, false);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), DarkGrey);
}

TEST(blending_skips_a_transparent_layer2_pixel) {
  Machine m(MachineType::Next);
  setupLayers(m, 0xE3); // the global transparency index
  setPriority(m, 6, false);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);
}

TEST(blending_mode_01_falls_back_to_the_priority_order) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);
  setPriority(m, 6, false);
  m.writeNextReg(0x68, 0x20); // bits 6:5 = 01 disables the blend
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Green);
}

TEST(layer2_priority_colour_sits_above_every_layer) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);
  setPriority(m, 4, false); // U S L: the ULA covers Layer 2
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);

  // A colour from the top half of the Layer 2 palette carries the priority
  // bit, which moves Layer 2 above all layers whatever 0x15 asks for.
  writePalette8(m, 0x85, 0x03);
  m.memWrite(0xC000, 0x85, 0);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Blue);
}

TEST(blending_with_the_tilemap_as_the_colour_keeps_the_ula_in_front) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);
  setPriority(m, 6, false);
  m.writeNextReg(0x68, 0x60); // bits 6:5 = 11: the tilemap is blended
  renderOneFrame(m);
  // The tilemap is off, so nothing is blended and the ULA half stays on top.
  CHECK_EQ(pixelAt(m, 0, 0), Red);
}

TEST(blending_layer2_priority_bit_lifts_the_pixel_above_the_sprites) {
  Machine m(MachineType::Next);
  setupLayers(m, 0x05);

  // A sprite covering the top left corner of the ULA, drawn in green.
  m.writeNextReg(0x43, 0x20);
  writePalette8(m, 0x01, 0x1C);
  m.ioWrite(0x303B, 0x00, 0);
  for (int i = 0; i < 256; ++i) m.ioWrite(0x005B, 0x01, 0);
  m.writeNextReg(0x34, 0x00);
  m.writeNextReg(0x35, 32);
  m.writeNextReg(0x36, 32);
  m.writeNextReg(0x37, 0x00);
  m.writeNextReg(0x38, 0x80);
  m.writeNextReg(0x39, 0x00);

  setPriority(m, 6, true);
  renderOneFrame(m);
  // Without the priority bit the sprite sits above the blend.
  CHECK_EQ(surfacePixel(m, 32, 32), Green);

  // Bit 7 of the Layer 2 colour survives the blend and lifts the pixel above
  // the sprite.
  m.memWrite(0xC000, 0x85, 0);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 32, 32), Red);
}
