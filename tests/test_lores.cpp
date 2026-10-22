// LoRes and Radastan: a 128x96 image stretched over the whole 256x192 ULA
// viewport, two display pixels per dot. NextReg 0x15 bit 7 enables LoRes and
// 0x6A bit 5 turns it into Radastan with four bits per dot.
//
// The expected addresses and palette indices below come from the core's
// lores.vhd, which is the only authoritative description of the mode.

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

// An 8-bit palette value is a RRRGGGBB colour, expanded the way the hardware
// does before it reaches the framebuffer.
u32 argb8(u8 value) {
  const u16 b2 = u16(value & 0x03);
  const u16 rgb = u16(((value & 0xE0u) << 1) | (((value & 0x1Cu) >> 2) << 3) |
                      ((b2 << 1) | (b2 != 0 ? 1 : 0)));
  return NextPalette::toArgb(rgb);
}

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

// The screen file is bank 5; the boot MMU maps its two halves at 0x4000/0x6000.
void mapScreenBank(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
}

// Fill the ULA palettes with a colour that encodes the palette index, so a test
// can tell exactly which entry the renderer picked without caring about the
// picture.
void seedPalettes(Machine& m) {
  // Bits 6:4 of 0x43 pick the palette a write lands in, so the second ULA
  // palette is 0x40 rather than the 0x02 that selects it for rendering.
  for (u8 control : {u8(0x00), u8(0x40)}) {
    m.writeNextReg(0x43, control);
    for (int entry = 0; entry < 256; ++entry) {
      writePalette8(m, u8(entry), u8(entry));
    }
  }
  m.writeNextReg(0x43, 0x00);
}

// A machine with a cleared bank 5, an index-encoding palette and LoRes off.
void prepareLoresMachine(Machine& m) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x4000; ++i) m.memWrite(u16(0x4000 + i), 0x00, 0);
  seedPalettes(m);
}

// Write a byte of the linear LoRes image, addressed the way the core does.
void pokeLores(Machine& m, u16 address, u8 value) {
  m.memWrite(u16(0x4000 + (address & 0x3FFF)), value, 0);
}

} // namespace

// LoRes is inert until NextReg 0x15 bit 7 is set, so a Next drawing an ordinary
// ULA screen is unaffected by the mode existing.
TEST(lores_is_off_by_default) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  // A classic ULA cell: the leftmost pixel lit on the first two rows.
  m.memWrite(0x4000, 0x80, 0);
  m.memWrite(0x4100, 0x80, 0);
  m.memWrite(0x5800, 0x07, 0); // ink 7 on paper 0

  renderOneFrame(m);

  // Ink 7 comes from entry 7 and paper 0 from entry 16, the two halves of the
  // ULA palette that the seeded colours map to themselves. One pixel wide is the
  // point: a LoRes dot would have covered two.
  CHECK_EQ(pixelAt(m, 0, 0), argb8(7));
  CHECK_EQ(pixelAt(m, 1, 0), argb8(16));
  CHECK_EQ(pixelAt(m, 0, 1), argb8(7));
  CHECK_EQ(pixelAt(m, 1, 1), argb8(16));
}

// A Radastan dot is four bits, and each byte holds two dots: the high nibble is
// the even dot of the pair and the low nibble the odd one.
TEST(lores_radastan_reads_both_nibbles) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80); // LoRes on
  m.writeNextReg(0x6A, 0x20); // Radastan, palette offset 0
  pokeLores(m, 0x0000, 0x31); // dot 0 = 3, dot 1 = 1

  renderOneFrame(m);

  // The whole palette index is just the nibble, because the offset is zero.
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x03));
  CHECK_EQ(pixelAt(m, 1, 0), argb8(0x03));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0x01));
  CHECK_EQ(pixelAt(m, 3, 0), argb8(0x01));
}

// Every dot covers two rows as well as two columns, and the next dot pair comes
// from the next byte.
TEST(lores_radastan_stretches_each_dot) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x31);
  pokeLores(m, 0x0001, 0x52); // dots 2 and 3

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x03));
  CHECK_EQ(pixelAt(m, 0, 1), argb8(0x03));
  CHECK_EQ(pixelAt(m, 1, 1), argb8(0x03));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0x01));
  CHECK_EQ(pixelAt(m, 3, 1), argb8(0x01));
  CHECK_EQ(pixelAt(m, 4, 0), argb8(0x05));
  CHECK_EQ(pixelAt(m, 6, 1), argb8(0x02));

  // Row 2 is the second image row, which starts 64 bytes in.
  CHECK_EQ(pixelAt(m, 0, 2), argb8(0x00));
  pokeLores(m, 0x0040, 0x70);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 2), argb8(0x07));
  CHECK_EQ(pixelAt(m, 1, 3), argb8(0x07));
}

// The 96 image rows are 64 bytes each, so the image ends at 0x1800 and the last
// image row reaches the bottom of the viewport.
TEST(lores_radastan_covers_all_96_rows) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, u16(95 * 64), 0xA0);

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 190), argb8(0x0A));
  CHECK_EQ(pixelAt(m, 1, 191), argb8(0x0A));
  // And nothing above it.
  CHECK_EQ(pixelAt(m, 0, 188), argb8(0x00));
}

// The palette offset is the whole high nibble, so Radastan can reach any of the
// sixteen blocks of the active ULA palette.
TEST(lores_radastan_palette_offset) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20 | 0x05); // Radastan, palette offset 5
  pokeLores(m, 0x0000, 0x73);

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x57));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0x53));
}

// With ULA+ enabled only the low two offset bits apply, and the high nibble
// becomes 11 so the dots address the top 64 entries that ULA+ owns.
TEST(lores_radastan_ulaplus_narrows_the_offset) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20 | 0x07); // offset 7, but only bits 1:0 survive
  m.writeNextReg(0x68, 0x08);        // ULA+ enable through the Next register
  pokeLores(m, 0x0000, 0x43);

  renderOneFrame(m);

  // The high nibble becomes "11" & offset(1:0), so the dots land in 0xC0-0xFF.
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0xC7));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0xC3));
}

// The ULA+ port enables the same narrowing, and ULANext turns it back off
// because the offset then means a plain palette block again.
TEST(lores_radastan_ulaplus_port_and_ulanext) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20 | 0x02);
  m.ioWrite(0xBF3B, 0x40, 0); // ULA+ register 0, block 1
  m.ioWrite(0xFF3B, 0x01, 0); // ULA+ enable
  pokeLores(m, 0x0000, 0x99);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0xCB));

  m.writeNextReg(0x43, 0x01); // ULANext on
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x29));
}

// NextReg 0x6A bit 4 XORs the Timex display file select, so the same image can
// be read from either 8K half of the bank without touching the SCLD register.
TEST(lores_radastan_display_file_select) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x40);
  pokeLores(m, 0x2000, 0xE0);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x04));

  // The XOR alone moves Radastan to the second file.
  m.writeNextReg(0x6A, 0x20 | 0x10);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x0E));

  // Setting the Timex bit as well cancels it again.
  m.ioWrite(0x00FF, 0x01, 0);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x04));

  // And the Timex bit on its own is enough to pick the second file.
  m.writeNextReg(0x6A, 0x20);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x0E));
}

// LoRes keeps eight bits per dot: the low nibble is the palette index directly
// and the high nibble is shifted by the palette offset.
TEST(lores_eight_bit_pixels) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x03); // plain LoRes, palette offset 3
  pokeLores(m, 0x0000, 0x21);

  renderOneFrame(m);

  // High nibble 2 + 3 = 5, low nibble 1 untouched.
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x51));
  CHECK_EQ(pixelAt(m, 1, 1), argb8(0x51));
  // Without the offset it is the raw byte.
  m.writeNextReg(0x6A, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x21));
}

// The high nibble add wraps inside four bits rather than carrying into the low
// nibble, which is what keeps the picture inside one 16-entry block.
TEST(lores_high_nibble_wraps) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x0A); // palette offset 10
  pokeLores(m, 0x0000, 0xF1);

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x91)); // 0xF + 0xA = 0x19 -> 9
}

// LoRes uses 128 bytes per image row and puts the second 48 rows 8K further up,
// so the picture spans 0x4000-0x57FF and 0x6000-0x77FF.
TEST(lores_row_layout_spans_two_halves) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x00);
  pokeLores(m, 0x0000, 0x11);  // row 0, dot 0
  pokeLores(m, 0x17FF, 0x22);  // row 47, dot 127
  pokeLores(m, 0x2000, 0x33);  // row 48, dot 0
  pokeLores(m, 0x37FF, 0x44);  // row 95, dot 127

  renderOneFrame(m);

  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x11));
  CHECK_EQ(pixelAt(m, 254, 94), argb8(0x22));
  CHECK_EQ(pixelAt(m, 0, 96), argb8(0x33));
  CHECK_EQ(pixelAt(m, 255, 191), argb8(0x44));
}

// 0x32 and 0x33 scroll LoRes in half-pixels, which for a 128 dot image means one
// display pixel per step, exactly like Layer 2.
TEST(lores_scroll_x_shifts_by_half_a_dot) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x4E); // dot 0 = 4, dot 1 = E

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x04));
  CHECK_EQ(pixelAt(m, 2, 0), argb8(0x0E));

  m.writeNextReg(0x32, 0x01);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x04)); // still the tail of dot 0
  CHECK_EQ(pixelAt(m, 1, 0), argb8(0x0E)); // dot 1 moved one pixel left

  m.writeNextReg(0x32, 0x02);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x0E));
}

TEST(lores_scroll_y_wraps_inside_the_viewport) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x40);      // row 0
  pokeLores(m, u16(95 * 64), 0xE0); // row 95

  pokeLores(m, u16(1 * 64), 0x70); // row 1

  m.writeNextReg(0x33, 0x02);
  renderOneFrame(m);
  // Two half-pixels up, so image row 1 is on the top line and row 0 has gone.
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x07));

  // Past 191 the core bumps the top two row bits instead of clamping, so the
  // scroll wraps back to the start of the image.
  m.writeNextReg(0x33, 0xC2);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x07));
  m.writeNextReg(0x33, 0xC0);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x04));
}

// NextReg 0x68 bit 7 blanks the ULA layer, and LoRes lives inside that layer,
// so the blank wins over the mode.
TEST(lores_honours_the_ula_blank) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x70);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0x07));

  // Blanking hands the pixel to the layers underneath, and with nothing else on
  // the border colour is what is left.
  m.writeNextReg(0x68, 0x80);
  renderOneFrame(m);
  const u32 borderPixel = m.framebuffer()[0];
  CHECK(pixelAt(m, 0, 0) == borderPixel);
  CHECK(borderPixel != argb8(0x07));
}

// The ULA palette select in 0x43 bit 1 moves LoRes to the second palette, the
// same way it moves the classic ULA.
TEST(lores_uses_the_selected_ula_palette) {
  Machine m(MachineType::Next);
  prepareLoresMachine(m);
  m.writeNextReg(0x15, 0x80);
  m.writeNextReg(0x6A, 0x20);
  pokeLores(m, 0x0000, 0x20);

  m.writeNextReg(0x43, 0x40); // writes land in the second ULA palette
  writePalette8(m, 0x02, 0xE0);
  m.writeNextReg(0x43, 0x02); // and bit 1 renders with it

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), argb8(0xE0));
}

// LoRes is a Next feature: a 48K machine ignores 0x15 entirely.
TEST(lores_is_next_only) {
  Machine m(MachineType::Spectrum48);
  m.writeNextReg(0x15, 0x80);
  renderOneFrame(m);
  // The screen file is empty, so the picture is the paper colour.
  CHECK_EQ(pixelAt(m, 0, 0), Machine::Palette[0]);
}
