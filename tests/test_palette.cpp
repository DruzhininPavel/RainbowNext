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

// A freshly reset Next renders whatever the ULA and Layer 2 leave in the
// framebuffer, so one frame is enough to inspect the result.
void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

u32 pixelAt(const Machine& m, int x, int y) {
  const int width = Machine::FrameWidth;
  return m.framebuffer()[size_t(Machine::BorderHeight + y) * size_t(width) +
                         size_t(Machine::BorderWidth + x)];
}

// The ULA screen is bank 5, which the Next maps as pages 10 and 11; pointing
// the MMU at them makes the screen reachable through the normal bus.
constexpr u16 ScreenBase = 0x4000;

void mapScreenBank(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
}

void clearUlaScreen(Machine& m) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(ScreenBase + i), 0x00, 0);
  for (u16 i = 0; i < 0x300; ++i) {
    m.memWrite(u16(ScreenBase + 0x1800 + i), 0x00, 0);
  }
}

// Ink 0 / paper 0 for every cell, with every pixel set to ink.
void fillUlaWithInk(Machine& m) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(ScreenBase + i), 0xFF, 0);
  for (u16 i = 0; i < 0x300; ++i) {
    m.memWrite(u16(ScreenBase + 0x1800 + i), 0x00, 0);
  }
}

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

// Every cell draws paper with no ink, so the whole ULA screen is one colour.
void fillUlaWithPaper(Machine& m, u8 attribute) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(ScreenBase + i), 0x00, 0);
  for (u16 i = 0; i < 0x300; ++i) {
    m.memWrite(u16(ScreenBase + 0x1800 + i), attribute, 0);
  }
}

} // namespace

TEST(palette_defaults_match_the_classic_spectrum_colours) {
  Machine m(MachineType::Next);
  const NextPalette& p = m.palette();

  CHECK_EQ(p.colour(NextPalette::Ula0, 0), 0xFF000000u); // black
  CHECK_EQ(p.colour(NextPalette::Ula0, 1), 0xFF0000DAu); // blue
  CHECK_EQ(p.colour(NextPalette::Ula0, 2), 0xFFDA0000u); // red
  CHECK_EQ(p.colour(NextPalette::Ula0, 7), 0xFFDADADAu); // white
  CHECK_EQ(p.colour(NextPalette::Ula0, 10), 0xFFFF0000u); // bright red
  CHECK_EQ(p.colour(NextPalette::Ula0, 15), 0xFFFFFFFFu); // bright white
  // The paper half of the ULA palette repeats the same sixteen colours.
  CHECK_EQ(p.colour(NextPalette::Ula0, 16), 0xFF000000u);
  CHECK_EQ(p.colour(NextPalette::Ula0, 23), 0xFFDADADAu);
}

TEST(palette_write8_stores_an_rrrgggbb_colour) {
  Machine m(MachineType::Next);

  writePalette8(m, 0x20, 0xE3); // red 7, green 0, blue 3
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x20), 0xFFFF00FFu);

  writePalette8(m, 0x21, 0x1C); // red 0, green 7, blue 0
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x21), 0xFF00FF00u);

  // 0x41 reads back the colour the index register points at, not the last byte
  // written: the boot ROM snapshots the palette with it.
  m.writeNextReg(0x40, 0x20);
  CHECK_EQ(m.readNextReg(0x41), 0xE3);
  m.writeNextReg(0x40, 0x21);
  CHECK_EQ(m.readNextReg(0x41), 0x1C);
}

TEST(palette_reads_back_the_nine_bit_colour) {
  Machine m(MachineType::Next);

  // 0x41 in 8-bit mode, written through the auto-incrementing index.
  m.writeNextReg(0x43, 0x00);
  m.writeNextReg(0x40, 0x10);
  m.writeNextReg(0x41, 0xE3); // red 7, green 0, blue 3

  // 0x44 reports bits 9:8 in 7:6 and bit 0 of the blue field in bit 0. The
  // eighth blue bit of an 8-bit write is duplicated into the ninth.
  m.writeNextReg(0x40, 0x10);
  CHECK_EQ(m.readNextReg(0x44), 0x01);

  // A 9-bit write takes the colour first and the ninth bit second, and keeps
  // the index parked on the entry.
  m.writeNextReg(0x43, 0x80); // bit 7 stops the auto-increment
  m.writeNextReg(0x40, 0x20);
  m.writeNextReg(0x44, 0xE0); // red 7, green 0, blue 0
  m.writeNextReg(0x44, 0x00);
  CHECK_EQ(m.readNextReg(0x41), 0xE0);
  CHECK_EQ(m.readNextReg(0x44), 0x00);

  m.writeNextReg(0x44, 0xE0);
  m.writeNextReg(0x44, 0x01); // blue LSB set
  CHECK_EQ(m.readNextReg(0x41), 0xE0);
  CHECK_EQ(m.readNextReg(0x44), 0x01);
}

TEST(palette_write8_auto_increments_the_index) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x43, 0x00); // bit 7 clear enables the auto-increment
  m.writeNextReg(0x40, 0x00);
  m.writeNextReg(0x41, 0xE3);
  m.writeNextReg(0x41, 0x1C);
  // Both writes stream from the index set by 0x40, so the counter ends at 2.
  CHECK_EQ(m.palette().index(), 0x02);
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x00), 0xFFFF00FFu);
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x01), 0xFF00FF00u);

  m.writeNextReg(0x40, 0x30);
  m.writeNextReg(0x43, 0x80); // bit 7 freezes the index
  m.writeNextReg(0x41, 0xE3);
  m.writeNextReg(0x41, 0x1C);
  CHECK_EQ(m.palette().index(), 0x30);
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x30), 0xFF00FF00u);
}

TEST(palette_write9_takes_two_bytes) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x40, 0x10);
  m.writeNextReg(0x44, 0xE3); // high byte, blue bits 3 and 2 set
  m.writeNextReg(0x44, 0x01); // low byte, blue LSB set -> blue 111
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x10), 0xFFFF00FFu);

  m.writeNextReg(0x40, 0x11);
  m.writeNextReg(0x44, 0xE3);
  m.writeNextReg(0x44, 0x00); // blue LSB clear -> blue 110
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x11), 0xFFFF00DAu);
}

TEST(palette_control_selects_the_palette_bank) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x43, 0x10); // bits 6:4 = 001 -> Layer 2 first palette
  writePalette8(m, 0x05, 0x1C);
  CHECK_EQ(m.palette().colour(NextPalette::Layer2First, 0x05), 0xFF00FF00u);
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x05), 0xFF00DADAu); // cyan

  m.writeNextReg(0x43, 0x40); // bits 6:4 = 100 -> ULA second palette
  writePalette8(m, 0x06, 0xE3);
  CHECK_EQ(m.palette().colour(NextPalette::Ula1, 0x06), 0xFFFF00FFu);
}

TEST(palette_changes_the_ula_output) {
  Machine m(MachineType::Next);
  fillUlaWithInk(m);

  writePalette8(m, 0x00, 0x1C); // ink 0 becomes bright green
  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), 0xFF00FF00u);
  CHECK_EQ(pixelAt(m, 255, 191), 0xFF00FF00u);
}

TEST(palette_changes_the_border_output) {
  Machine m(MachineType::Next);

  m.ioWrite(0x00FE, 0x07, 0); // white border
  // Entry 16 + 7 is the paper form of the border colour.
  writePalette8(m, 0x17, 0x03); // red 0, green 0, blue 3
  renderOneFrame(m);

  CHECK_EQ(m.framebuffer()[0], 0xFF0000FFu);
}

TEST(layer2_is_off_until_register_69_enables_it) {
  Machine m(MachineType::Next);
  clearUlaScreen(m);
  m.memWrite(0xC000, 0x05, 0); // Layer 2 bank 0, first pixel

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF000000u); // ULA paper, Layer 2 still off
}

TEST(layer2_draws_over_the_ula) {
  Machine m(MachineType::Next);
  clearUlaScreen(m);
  m.memWrite(0xC000, 0x05, 0); // Layer 2 pixel (0, 0) = entry 5

  m.writeNextReg(0x12, 0x00); // active bank 0
  m.writeNextReg(0x69, 0x80); // enable Layer 2
  m.writeNextReg(0x43, 0x10);
  writePalette8(m, 0x05, 0x1C);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF00FF00u);
  CHECK_EQ(pixelAt(m, 1, 0), 0xFF000000u); // untouched Layer 2 pixel is black
}

TEST(layer2_transparent_pixels_show_the_ula) {
  Machine m(MachineType::Next);
  fillUlaWithInk(m);
  m.memWrite(0xC000, 0xE3, 0); // the global transparency value

  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF000000u); // ULA ink 0, not Layer 2
}

TEST(layer2_honours_the_scroll_registers) {
  Machine m(MachineType::Next);
  clearUlaScreen(m);
  m.memWrite(0xC000, 0x05, 0);

  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80);
  m.writeNextReg(0x43, 0x10);
  writePalette8(m, 0x05, 0x1C);
  m.writeNextReg(0x16, 0xFF); // scroll a full row, wrapping to the last column

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF000000u); // source column 255 is untouched
  CHECK_EQ(pixelAt(m, 1, 0), 0xFF00FF00u); // source column 0 is entry 5
}

TEST(layer2_stays_below_the_ula_when_the_priority_says_so) {
  Machine m(MachineType::Next);
  fillUlaWithInk(m);
  m.memWrite(0xC000, 0x05, 0);

  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80);
  m.writeNextReg(0x43, 0x10);
  writePalette8(m, 0x05, 0x1C);
  m.writeNextReg(0x15, 0x10); // bits 4:2 = 100 -> ULA over Layer 2

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF000000u);
}

TEST(palette_reset_restores_the_defaults) {
  Machine m(MachineType::Next);
  writePalette8(m, 0x01, 0x1C);
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x01), 0xFF00FF00u);

  m.reset();
  CHECK_EQ(m.palette().colour(NextPalette::Ula0, 0x01), 0xFF0000DAu);
  CHECK_EQ(m.palette().index(), 0);
}

TEST(layer2_clip_window_hides_pixels_outside_it) {
  Machine m(MachineType::Next);
  clearUlaScreen(m);
  for (u16 x = 0; x < 8; ++x) m.memWrite(u16(0xC000 + x), 0x05, 0);

  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80);
  m.writeNextReg(0x43, 0x10);
  writePalette8(m, 0x05, 0x1C);
  // Four writes: X1, X2, Y1, Y2. Only columns 2 to 4 stay visible.
  m.writeNextReg(0x18, 2);
  m.writeNextReg(0x18, 4);
  m.writeNextReg(0x18, 0);
  m.writeNextReg(0x18, 191);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 1, 0), 0xFF000000u);
  CHECK_EQ(pixelAt(m, 2, 0), 0xFF00FF00u);
  CHECK_EQ(pixelAt(m, 4, 0), 0xFF00FF00u);
  CHECK_EQ(pixelAt(m, 5, 0), 0xFF000000u);
}

TEST(layer2_clip_window_resets_to_the_full_screen) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x18, 10);
  m.reset();

  Machine reference(MachineType::Next);
  clearUlaScreen(reference);
  m.memWrite(0xC000, 0x05, 0);
  m.writeNextReg(0x12, 0x00);
  m.writeNextReg(0x69, 0x80);
  m.writeNextReg(0x43, 0x10);
  writePalette8(m, 0x05, 0x1C);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFF00FF00u);
}

TEST(ula_clip_window_hides_pixels_outside_it) {
  Machine m(MachineType::Next);
  fillUlaWithPaper(m, 0x10); // paper 2, which is red

  // Four writes: X1, X2, Y1, Y2. Only columns 2 to 4 stay visible.
  m.writeNextReg(0x1A, 2);
  m.writeNextReg(0x1A, 4);
  m.writeNextReg(0x1A, 0);
  m.writeNextReg(0x1A, 191);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 1, 0), 0xFF000000u);
  CHECK_EQ(pixelAt(m, 2, 0), 0xFFDA0000u);
  CHECK_EQ(pixelAt(m, 4, 0), 0xFFDA0000u);
  CHECK_EQ(pixelAt(m, 5, 0), 0xFF000000u);
  // Rows outside the window go the same way.
  CHECK_EQ(pixelAt(m, 3, 0), 0xFFDA0000u);
}

TEST(ula_clip_window_resets_to_the_full_screen) {
  Machine m(MachineType::Next);
  fillUlaWithPaper(m, 0x10);
  m.writeNextReg(0x1A, 0xBF);
  m.reset();

  fillUlaWithPaper(m, 0x10);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), 0xFFDA0000u);
  CHECK_EQ(pixelAt(m, 255, 191), 0xFFDA0000u);
}

TEST(ula_clip_index_resets_through_register_1c) {
  Machine m(MachineType::Next);
  fillUlaWithPaper(m, 0x10);
  m.writeNextReg(0x1A, 0); // only the first of the four writes

  // Bit 2 rewinds the ULA window so the next four writes start at X1 again.
  m.writeNextReg(0x1C, 0x04);
  for (const u8 value : {u8(2), u8(4), u8(0), u8(191)}) {
    m.writeNextReg(0x1A, value);
  }

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 1, 0), 0xFF000000u);
  CHECK_EQ(pixelAt(m, 3, 0), 0xFFDA0000u);
}
