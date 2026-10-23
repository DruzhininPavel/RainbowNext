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

// Bank 5 is the ULA screen and the boot MMU maps its two halves at 0x4000 and
// 0x6000, so the second display file the Timex modes use is reachable at once.
constexpr u16 ScreenBase = 0x4000;
constexpr u16 AltBase = 0x6000;

void clearScreen(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
  for (u16 i = 0; i < 0x4000; ++i) m.memWrite(u16(ScreenBase + i), 0x00, 0);
}

// Port 0xFF selects the Timex display mode in bits 2:0 and the two hi-res
// colours in bits 5:3. On the Next the address only reaches the SCLD while
// Timex mode is enabled in NR 0x82 bit 0, so the helper turns it on first.
void writeTimex(Machine& m, u8 value) {
  m.writeNextReg(0x82, u8(m.readNextReg(0x82) | 0x01));
  m.ioWrite(0x00FF, value, 0);
}

// The classic ULA palette the Next starts with, as 8-bit ARGB.
const u32 Black = 0xFF000000u;
// The classic machines draw with their own sixteen-colour table.
const u32 ClassicWhite = Machine::Palette[7];
const u32 BrightWhite = 0xFFFFFFFFu;
const u32 BrightRed = 0xFFFF0000u;
const u32 BrightBlue = 0xFF0000FFu;
const u32 BrightCyan = 0xFF00FFFFu;

} // namespace

TEST(timex_second_display_file_moves_the_screen_up_8k) {
  Machine m(MachineType::Next);
  clearScreen(m);

  // The first file shows ink 0 on paper 0 with every pixel lit, which is black.
  m.memWrite(ScreenBase, 0xFF, 0);
  m.memWrite(u16(ScreenBase + 0x1800), 0x00, 0);
  // The second file holds bright white ink instead.
  m.memWrite(AltBase, 0xFF, 0);
  m.memWrite(u16(AltBase + 0x1800), 0x47, 0);

  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);

  writeTimex(m, 0x01); // ALTDFILE
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);

  writeTimex(m, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);
}

TEST(timex_hi_colour_takes_the_attribute_from_the_second_file) {
  Machine m(MachineType::Next);
  clearScreen(m);

  // Pixels from the first file, one attribute per cell from the second file at
  // the same interleaved offset the pixels use.
  m.memWrite(ScreenBase, 0xFF, 0);
  m.memWrite(u16(ScreenBase + 1), 0xFF, 0);
  m.memWrite(AltBase, 0x47, 0);          // bright, paper 0, ink 7
  m.memWrite(u16(AltBase + 1), 0x4A, 0); // bright, paper 1, ink 2

  writeTimex(m, 0x02); // EXTCOLOUR
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);
  CHECK_EQ(pixelAt(m, 8, 0), BrightRed);
  // The cells either side of the second one keep the attribute of the cell
  // their pixels belong to.
  CHECK_EQ(pixelAt(m, 7, 0), BrightWhite);

  // Without the mode the same cells read their attributes from 0x5800, which is
  // still zero, so both draw black ink.
  writeTimex(m, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);
  CHECK_EQ(pixelAt(m, 8, 0), Black);
}

TEST(timex_hi_res_builds_512_pixel_lines_from_two_files) {
  Machine m(MachineType::Next);
  clearScreen(m);

  // The first file holds the even hi-res columns and the second the odd ones.
  m.memWrite(ScreenBase, 0xFF, 0);
  m.memWrite(AltBase, 0x00, 0);

  writeTimex(m, 0x06); // HIRES with paper black on ink white
  renderOneFrame(m);
  // Pairs 0-3 cover hi-res pixels 0-7, all of them ink.
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);
  CHECK_EQ(pixelAt(m, 3, 0), BrightWhite);
  // Pairs 4-7 cover hi-res pixels 8-15, which come from the second file.
  CHECK_EQ(pixelAt(m, 4, 0), Black);
  CHECK_EQ(pixelAt(m, 7, 0), Black);
}

TEST(timex_hi_res_averages_the_two_columns_it_covers) {
  Machine m(MachineType::Next);
  clearScreen(m);

  // One lone hi-res pixel in the middle of a dark line.
  m.memWrite(ScreenBase, 0x00, 0);
  m.memWrite(AltBase, 0x80, 0);

  writeTimex(m, 0x06);
  renderOneFrame(m);
  // Hi-res pixel 8 is white and its neighbour is black, so the output pixel
  // that covers both is half lit in every channel.
  CHECK_EQ(pixelAt(m, 4, 0), 0xFF808080u);
  CHECK_EQ(pixelAt(m, 3, 0), Black);
  CHECK_EQ(pixelAt(m, 5, 0), Black);
}

TEST(timex_hi_res_colours_come_from_bits_3_to_5) {
  Machine m(MachineType::Next);
  clearScreen(m);

  // A dark line, so every pixel takes the paper colour.
  writeTimex(m, u8(0x06 | (0x01 << 3))); // paper blue, ink yellow
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightBlue);
  // The border takes the paper colour of the same pair.
  CHECK_EQ(m.framebuffer()[0], BrightBlue);
}

TEST(timex_hi_res_ink_is_the_complement_of_the_paper_colour) {
  Machine m(MachineType::Next);
  clearScreen(m);

  m.memWrite(ScreenBase, 0xFF, 0); // every even column lit
  m.memWrite(AltBase, 0xFF, 0);    // every odd column lit
  writeTimex(m, u8(0x06 | (0x02 << 3))); // paper red, ink cyan
  renderOneFrame(m);
  // Both hi-res pixels of every pair are ink, which is the complement.
  CHECK_EQ(pixelAt(m, 0, 0), BrightCyan);
  CHECK_EQ(m.framebuffer()[0], BrightRed);
}

TEST(timex_modes_are_ignored_on_the_classic_machines) {
  Machine m(MachineType::Spectrum48);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(0x4000 + i), 0xFF, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(0x5800 + i), 0x07, 0);

  // The port is not decoded at all, so the screen keeps its attributes.
  writeTimex(m, 0x06);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), ClassicWhite);
}

TEST(timex_reset_returns_to_the_standard_screen) {
  Machine m(MachineType::Next);
  clearScreen(m);
  m.memWrite(AltBase, 0xFF, 0);
  m.memWrite(u16(AltBase + 0x1800), 0x47, 0);

  writeTimex(m, 0x01);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);

  m.reset();
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
  m.memWrite(ScreenBase, 0xFF, 0);
  m.memWrite(u16(ScreenBase + 0x1800), 0x47, 0);
  renderOneFrame(m);
  // The reset emptied both files and put the mode back to standard, so the
  // first file is what shows.
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);
}

// On the Next the Timex register shares its address with the firmware's palette
// traffic, so NR 0x82 bit 0 decides whether the write reaches the SCLD at all.
TEST(timex_port_needs_the_next_enable_bit) {
  Machine m(MachineType::Next);
  clearScreen(m);
  m.memWrite(AltBase, 0xFF, 0);
  m.memWrite(u16(AltBase + 0x1800), 0x47, 0);

  m.writeNextReg(0x82, u8(m.readNextReg(0x82) & 0xFE));
  m.ioWrite(0x00FF, 0x01, 0); // ALTDFILE, but the SCLD never sees it
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);

  m.writeNextReg(0x82, u8(m.readNextReg(0x82) | 0x01));
  m.ioWrite(0x00FF, 0x01, 0);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);
}

// NR 0x69 is the Next-register view of the same controls, so writing it moves
// the classic ports and reading it back recomposes them.
TEST(timex_next_reg_mirrors_the_scld_register) {
  Machine m(MachineType::Next);
  clearScreen(m);
  m.memWrite(AltBase, 0xFF, 0);
  m.memWrite(u16(AltBase + 0x1800), 0x47, 0);

  m.writeNextReg(0x69, 0x01); // ALTDFILE through the Next register
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), BrightWhite);
  CHECK_EQ(m.readNextReg(0x69) & 0x3F, 0x01);

  m.writeNextReg(0x69, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);
  CHECK_EQ(m.readNextReg(0x69) & 0x3F, 0x00);
}
