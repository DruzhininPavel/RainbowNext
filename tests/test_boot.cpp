#include "test_framework.h"

#include <string>
#include <vector>

#include "core/machine.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u32;
using rainbow::u64;
using rainbow::u8;

namespace {

// Hand-assembled 48K program:
//   LD A,02h / OUT (FEh),A     ; border = 2 (red)
//   LD HL,4000h / LD A,FFh / LD (HL),A   ; first 8 pixels of the screen on
//   LD HL,5800h / LD A,07h / LD (HL),A   ; ink = 7 (white), paper = 0 (black)
//   JR $                       ; spin forever
const std::vector<u8> kProgram = {
    0x3E, 0x02,       0xD3, 0xFE,
    0x21, 0x00, 0x40, 0x3E, 0xFF, 0x77,
    0x21, 0x00, 0x58, 0x3E, 0x07, 0x77,
    0x18, 0xFE,
};

} // namespace

TEST(boot_runs_program_from_rom) {
  Machine machine(MachineType::Spectrum48);
  CHECK(machine.loadRomData(kProgram.data(), kProgram.size(), 0));
  machine.reset();
  CHECK(machine.romLoaded());

  machine.runFrame();

  CHECK_EQ(machine.frames(), 1);
  CHECK_EQ(machine.border(), 2);

  // The program really executed: RAM now holds the pixel and attribute bytes.
  CHECK_EQ(machine.memRead(0x4000, 0), 0xFF);
  CHECK_EQ(machine.memRead(0x5800, 0), 0x07);
}

TEST(boot_renders_screen_and_border) {
  Machine machine(MachineType::Spectrum48);
  machine.loadRomData(kProgram.data(), kProgram.size(), 0);
  machine.reset();
  machine.runFrame();

  const u32* fb = machine.framebuffer();
  constexpr int W = Machine::FrameWidth;
  constexpr int B = Machine::BorderWidth;
  constexpr int BH = Machine::BorderHeight;

  const u32 borderPixel = fb[0];
  CHECK_EQ(borderPixel, Machine::Palette[2]);

  // Screen row 0, first column: all eight pixels are ink (white).
  const u32 ink = fb[size_t(BH) * W + B];
  CHECK_EQ(ink, Machine::Palette[7]);
  CHECK_EQ(fb[size_t(BH) * W + B + 7], Machine::Palette[7]);

  // Second character cell was left empty, so it stays paper (black).
  CHECK_EQ(fb[size_t(BH) * W + B + 8], Machine::Palette[0]);

  // The border columns of a screen row use the current border colour.
  CHECK_EQ(fb[size_t(BH) * W], Machine::Palette[2]);
}

TEST(boot_is_deterministic) {
  auto runOnce = [] {
    Machine machine(MachineType::Spectrum48);
    machine.loadRomData(kProgram.data(), kProgram.size(), 0);
    machine.reset();
    for (int i = 0; i < 3; ++i) machine.runFrame();
    return machine.cpu().cycles();
  };
  const u64 cycles = runOnce();
  CHECK_EQ(cycles, runOnce());
  // Instruction-granular stepping may overshoot the frame end by one instruction.
  CHECK(cycles >= 3ull * 69888ull);
  CHECK(cycles < 3ull * 69888ull + 32ull);
}

TEST(boot_halt_stops_the_cpu_until_interrupt) {
  // LD A,01h / OUT (FEh),A / HALT / JR $
  const std::vector<u8> program = {0x3E, 0x01, 0xD3, 0xFE, 0x76, 0x18, 0xFE};
  Machine machine(MachineType::Spectrum48);
  machine.loadRomData(program.data(), program.size(), 0);
  machine.reset();
  machine.runFrame();

  CHECK(machine.cpu().regs().halted);
  CHECK_EQ(machine.border(), 1);

  // A second frame delivers the frame interrupt, which resumes the CPU.
  machine.runFrame();
  CHECK_EQ(machine.frames(), 2);
}

namespace {

// Pixel address of the first byte of a text cell, using the ULA's interleaved
// layout (third of the screen, then pixel line, then character row).
rainbow::u16 screenCellAddress(int textRow, int column, int pixelLine) {
  const int y = textRow * 8 + pixelLine;
  return rainbow::u16(0x4000 + ((y & 0xC0) << 5) + ((y & 0x07) << 8) +
                     ((y & 0x38) << 2) + column);
}

bool textRowHasInk(Machine& machine, int textRow) {
  for (int column = 0; column < 32; ++column)
    for (int line = 0; line < 8; ++line)
      if (machine.memRead(screenCellAddress(textRow, column, line), 0)) return true;
  return false;
}

// Recovers the text of one character row by matching each cell against the ROM
// character set, so the test does not depend on how glyphs are rasterised.
std::string screenRowText(Machine& machine, int textRow) {
  constexpr rainbow::u16 kChars = 0x3D00;
  std::string text;
  for (int column = 0; column < 32; ++column) {
    const rainbow::u16 address = screenCellAddress(textRow, column, 0);
    int bestChar = '?';
    int bestDistance = 8 * 8 + 1;
    for (int ch = 32; ch < 128; ++ch) {
      int distance = 0;
      for (int line = 0; line < 8; ++line) {
        const u8 glyph = machine.memRead(kChars + rainbow::u16((ch - 32) * 8 + line), 0);
        const u8 pixels = machine.memRead(address + rainbow::u16(line << 8), 0);
        distance += __builtin_popcount(pixels ^ glyph);
      }
      if (distance < bestDistance) {
        bestDistance = distance;
        bestChar = ch;
      }
    }
    text.push_back(char(bestChar));
  }
  return text;
}

void tapKey(Machine& machine, int row, int column) {
  machine.keyboard().setKey(row, column, true);
  for (int i = 0; i < 4; ++i) machine.runFrame();
  machine.keyboard().setKey(row, column, false);
  for (int i = 0; i < 4; ++i) machine.runFrame();
}

} // namespace

// Golden test against the real 48K ROM: it is not shipped with the repository,
// so the test skips silently when the file is absent (e.g. in CI).
TEST(boot_real_48k_rom_prints_copyright_message) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;

  for (int i = 0; i < 120; ++i) machine.runFrame();

  // White border, white paper (attribute 0x38), and the boot message on the
  // bottom text row. This also proves the ROM left its memory test loop.
  CHECK_EQ(machine.border(), 7);
  CHECK_EQ(machine.memRead(0x5800, 0), 0x38);
  CHECK_EQ(machine.memRead(0x5AFF, 0), 0x38);
  CHECK(textRowHasInk(machine, 23));
  for (int row = 0; row < 23; ++row) CHECK(!textRowHasInk(machine, row));

  // The framebuffer must actually contain ink pixels, not just the border.
  int darkPixels = 0;
  const u32* fb = machine.framebuffer();
  for (u32 i = 0; i < 320u * 256u; ++i)
    if ((fb[i] & 0xFFFFFFu) == 0) ++darkPixels;
  CHECK(darkPixels > 100);
}

// Golden test of the full BASIC round trip: type `PRINT 42`, press ENTER and
// check that the ROM evaluates it. This exercises the interpreter, the
// expression evaluator, the number-to-string conversion and the lower-screen
// report channel on top of everything the copyright test already covers.
TEST(boot_real_48k_rom_runs_basic_program) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;

  for (int i = 0; i < 130; ++i) machine.runFrame();

  // Keyboard matrix rows: 5 = "P O I U Y", 3 = "1 2 3 4 5", 6 = "ENTER ...".
  tapKey(machine, 5, 0); // P -> PRINT keyword
  tapKey(machine, 3, 3); // 4
  tapKey(machine, 3, 1); // 2

  // The editor echoes the keywords on the bottom line with a cursor attribute
  // (0x38 with bit 7 set) on the character under the cursor.
  const std::string typed = screenRowText(machine, 23);
  CHECK(typed.rfind("PRINT 42", 0) == 0);

  tapKey(machine, 6, 0); // ENTER
  for (int i = 0; i < 10; ++i) machine.runFrame();

  // `PRINT 42` writes to the upper screen at the current print position, which
  // is the top-left corner right after boot.
  CHECK(screenRowText(machine, 0).rfind("42", 0) == 0);

  // The report "0 OK, 0:1" goes to the lower screen; 0:1 is line 0, statement 1.
  CHECK(screenRowText(machine, 23).rfind("0 OK, 0:1", 0) == 0);
}
