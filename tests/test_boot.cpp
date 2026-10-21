#include "test_framework.h"

#include <vector>

#include "core/machine.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::u32;
using mynext::u64;
using mynext::u8;

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
