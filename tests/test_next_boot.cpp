// The Next boot ROM is the hardest thing the core has to run: before it will
// print anything it checks 2 MB of RAM and works out how much is there. These
// tests pin the invariants that took the longest to establish, so a regression
// in the MMU or in Z80N shows up as a failure here instead of as a machine that
// quietly reboots forever.

#include <cstdio>
#include <string>
#include <vector>

#include "test_framework.h"

#include "core/machine.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u16;
using rainbow::u8;

namespace {

std::vector<u8> readFile(const std::string& path) {
  std::vector<u8> data;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return data;
  u8 buffer[0x4000];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
    data.insert(data.end(), buffer, buffer + got);
  }
  std::fclose(file);
  return data;
}

std::string romPath(const char* name) {
  return std::string(RAINBOW_SOURCE_DIR) + "/roms/" + name;
}

// The board ROM is a 64 KiB image holding four 16 KiB banks.
bool loadNextRom(Machine& machine) {
  const std::vector<u8> rom = readFile(romPath("next.rom"));
  if (rom.size() != 0x10000) return false;
  return machine.loadRomImageData(rom.data(), rom.size());
}

// Runs until the pc has been seen at `address` `target` times, and reports how
// many times it was seen. Stops early so a passing test does not keep running.
int countVisits(Machine& machine, u16 address, int target, long limit) {
  int seen = 0;
  for (long i = 0; i < limit && seen < target; ++i) {
    if (machine.cpu().regs().pc == address) ++seen;
    machine.stepInstruction();
  }
  return seen;
}

} // namespace

// 0x018E is the first instruction after the RAM probe succeeds: the probe
// fills memory with 0xBB markers, scans it back with LDDR, and only falls
// through to the page-counting loop when every pass matched. The ROM runs the
// whole boot sequence seven times before it settles, so seven arrivals at
// 0x018E mean the probe - and with it the MMU, the 2 MB RAM size and the Z80N
// instructions the setup table uses - is behaving. The budget covers a little
// over two million instructions, which is what the seven passes cost; it is
// deliberately generous because contention shifts how many instructions fit
// into the waits between them.
TEST(boot_real_next_rom_completes_its_ram_probe) {
  Machine machine(MachineType::Next);
  if (!loadNextRom(machine)) return;
  CHECK_EQ(countVisits(machine, 0x018E, 7, 2500000), 7);
}

// The boot ROM is handed the machine in configuration mode, which is the only
// state in which the machine configuration registers accept writes. Register
// 0x06 bit 2 is stored rather than composed, so the ROM reads back the bit the
// firmware left in it; a machine that never entered the mode answers with a
// bit it does not expect and takes a different path from the first read on.
TEST(boot_next_starts_in_configuration_mode) {
  Machine machine(MachineType::Next);
  CHECK_EQ(machine.readNextReg(0x06) & 0x04, 0x00); // nothing has set it yet
  machine.writeNextReg(0x0A, 0xB1);                 // the mode is on regardless
  CHECK_EQ(machine.readNextReg(0x0A), 0xB1);
}

TEST(boot_spectrum_has_no_configuration_mode) {
  Machine machine(MachineType::Spectrum48);
  machine.writeNextReg(0x0A, 0xE0);
  CHECK_EQ(machine.readNextReg(0x0A), 0x00); // bits 7:5 are not reachable
}

// The low three bits of register 0x03 are the mode selector: 7 enters, any
// other non-zero value leaves and 0 is not a command at all.
TEST(boot_configuration_mode_is_left_by_a_machine_type_write) {
  Machine machine(MachineType::Next);
  machine.writeNextReg(0x03, 0xB3); // leave
  machine.writeNextReg(0x0A, 0xE0);
  CHECK_EQ(machine.readNextReg(0x0A), 0x00); // the configuration bits are frozen
  machine.writeNextReg(0x03, 0x00);          // not a command: still out
  machine.writeNextReg(0x0A, 0xE0);
  CHECK_EQ(machine.readNextReg(0x0A), 0x00);
  machine.writeNextReg(0x03, 0xB7); // 7 enters again
  machine.writeNextReg(0x0A, 0xE0);
  CHECK_EQ(machine.readNextReg(0x0A), 0xE0);
}

// Bits 7:5 of register 0x0A are the machine configuration and are only
// reachable while the mode is on; the divMMC automap switch below them is not.
TEST(boot_configuration_mode_gates_register_0a) {
  Machine machine(MachineType::Next);
  machine.writeNextReg(0x0A, 0xB1);
  CHECK_EQ(machine.readNextReg(0x0A), 0xB1);
  machine.writeNextReg(0x03, 0xB3);
  machine.writeNextReg(0x0A, 0xE0);
  CHECK_EQ(machine.readNextReg(0x0A), 0xA0);
  // The divMMC automap switch below the configuration bits is always writable.
  machine.writeNextReg(0x0A, 0x10);
  CHECK_EQ(machine.readNextReg(0x0A), 0xB0);
}

// The firmware is what hands a cold machine over to NextZXOS: it programs the
// FPGA through the register file and finishes with a soft reset. It runs as
// the last piece of a Next ROM set is loaded, so these values are what a
// program finds at its first instruction rather than what a reset leaves.
TEST(boot_next_firmware_programs_the_register_file) {
  Machine machine(MachineType::Next);
  if (!loadNextRom(machine)) return;
  std::string error;
  CHECK(machine.loadDivMmcRom(romPath("nxtmmc.rom"), error));

  CHECK_EQ(machine.nextRegs().read(0x03), 0x33); // machine type: Next, 3.5 MHz
  CHECK_EQ(machine.nextRegs().read(0x04), 0x03); // ROM trap: NextZXOS
  CHECK_EQ(machine.nextRegs().read(0x0A), 0x11); // Next, divMMC automap off
  CHECK_EQ(machine.nextRegs().read(0x10), 0x04); // Layer 2 at 256x192
  CHECK_EQ(machine.nextRegs().read(0x11), 0x00); // ...and its scroll MSB
  CHECK_EQ(machine.nextRegs().read(0x89), 0x8F); // the power-on palette index
  CHECK_EQ(machine.nextRegs().read(0x8B), 0x00);
  // The firmware writes 0x9D to 0x06 while the mode is on, so bit 2 is set in
  // the file and the soft reset keeps it: the boot ROM's read-modify-write of
  // the register sees the bit and writes 0x04 rather than 0x00.
  CHECK_EQ(machine.readNextReg(0x06), 0xBD);
}

// Only the Next runs a firmware; every other machine just gets a divMMC ROM.
TEST(boot_spectrum_is_not_touched_by_a_divmmc_rom) {
  Machine machine(MachineType::Spectrum128);
  std::string error;
  CHECK(machine.loadDivMmcRom(romPath("nxtmmc.rom"), error));
  // Only the firmware programs these, so a plain 128K still shows the power-on
  // state rather than what a Next would have been configured with.
  CHECK_EQ(machine.nextRegs().read(0x04), 0x00);
  CHECK_EQ(machine.nextRegs().read(0x0A), 0x01);
  CHECK_EQ(machine.nextRegs().read(0x8B), 0x00);
}
