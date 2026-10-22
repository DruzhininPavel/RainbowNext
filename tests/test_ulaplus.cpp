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

constexpr u16 ScreenBase = 0x4000;

// The screen is bank 5, which the boot MMU maps as pages 10 and 11.
void mapScreenBank(Machine& m) {
  m.writeNextReg(0x54, 10);
  m.writeNextReg(0x55, 11);
}

// One cell in the top left corner of the screen and nothing else.
void writeCell(Machine& m, u8 attr, u8 pixels) {
  mapScreenBank(m);
  for (u16 i = 0; i < 0x1800; ++i) m.memWrite(u16(ScreenBase + i), 0x00, 0);
  for (u16 i = 0; i < 0x300; ++i) m.memWrite(u16(ScreenBase + 0x1800 + i), 0x00, 0);
  m.memWrite(ScreenBase, pixels, 0);
  m.memWrite(u16(ScreenBase + 0x1800), attr, 0);
}

// ULA+ has its own pair of ports: one selects a register, the other carries the
// data.
void writeUlaPlus(Machine& m, u8 index, u8 value) {
  m.ioWrite(0xBF3B, u8(index & 0x3F), 0);
  m.ioWrite(0xFF3B, value, 0);
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
const u32 Red = argb8(0xE0);
const u32 Green = argb8(0x1C);
const u32 Blue = argb8(0x03);

} // namespace

TEST(ulaplus_writes_the_ula_palette_through_its_own_ports) {
  Machine m(MachineType::Next);
  // Bright ink 0 lives at entry 16 of the ULA palette, which is where the
  // classic mode keeps the ink of the second half of the screen.
  writeCell(m, 0x40, 0xFF);

  writeUlaPlus(m, 0x10, 0xE0); // bright red
  m.writeNextReg(0x68, 0x08);  // ULA+ on
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);

  // The data port reads back what was written.
  m.ioWrite(0xBF3B, 0x10, 0);
  CHECK_EQ(m.ioRead(0xFF3B, 0), 0xE0);
}

TEST(ulaplus_stays_off_until_it_is_enabled) {
  Machine m(MachineType::Next);
  writeCell(m, 0x40, 0xFF);
  writeUlaPlus(m, 0x10, 0xE0);

  // Without ULA+ the same attribute reads the classic bright black of entry 8.
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Black);

  // The mode group of the register port switches it on by itself.
  m.ioWrite(0xBF3B, 0x40, 0);
  m.ioWrite(0xFF3B, 0x01, 0);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);
  m.ioWrite(0xBF3B, 0x40, 0);
  CHECK_EQ(m.ioRead(0xFF3B, 0), 0x01);
}

TEST(ulaplus_selects_the_block_of_sixteen_by_flash_and_bright) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x68, 0x08);

  // Bright ink 0 is the first entry of the second block.
  writeUlaPlus(m, 0x10, 0xE0);
  writeCell(m, 0x40, 0xFF);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);

  // The flash bit picks the third block and does not flash in this mode.
  writeUlaPlus(m, 0x20, 0x1C);
  writeCell(m, 0x80, 0xFF);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Green);
}

TEST(ulaplus_paper_uses_the_upper_half_of_the_block) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x68, 0x08);

  // Paper 2 is entry 10 of the first block.
  writeUlaPlus(m, 0x0A, 0x03);
  writeCell(m, 0x10, 0x00); // ink 0, paper 2, every pixel dark
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Blue);
}

TEST(ulaplus_border_is_the_first_paper_colour) {
  Machine m(MachineType::Next);
  writeCell(m, 0x00, 0x00);
  m.writeNextReg(0x68, 0x08);

  writeUlaPlus(m, 0x08, 0x1C); // green
  m.ioWrite(0x00FE, 0x03, 0);
  renderOneFrame(m);
  CHECK_EQ(m.framebuffer()[0], Green);
}

TEST(ulanext_turns_the_attribute_into_a_pair_of_palette_indices) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x42, 0x0F); // sixteen inks, sixteen papers
  m.writeNextReg(0x43, 0x01); // ULANext on, ULA first palette

  m.writeNextReg(0x40, 0x05);
  m.writeNextReg(0x41, 0xE0); // ink 5 is red
  m.writeNextReg(0x40, 0xA0);
  m.writeNextReg(0x41, 0x1C); // paper 160 is green

  // The low nibble of the attribute is ink 5 and the high one paper 160.
  writeCell(m, 0x25, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Green);

  writeCell(m, 0x25, 0xFF);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);
}

TEST(ulanext_full_ink_takes_the_paper_colour_from_register_0x4a) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x42, 0xFF); // the whole attribute is one ink index
  m.writeNextReg(0x43, 0x01);
  m.writeNextReg(0x4A, 0x03); // fallback colour 3, bright blue

  m.writeNextReg(0x40, 0x25);
  m.writeNextReg(0x41, 0xE0); // ink 37 is red
  m.writeNextReg(0x40, 0x13);
  m.writeNextReg(0x41, 0x1C); // paper 19 is green

  writeCell(m, 0x25, 0xFF);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Red);

  writeCell(m, 0x25, 0x00);
  renderOneFrame(m);
  CHECK_EQ(pixelAt(m, 0, 0), Green);
}
