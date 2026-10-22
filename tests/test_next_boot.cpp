// The Next boot ROM is the hardest thing the core has to run: before it will
// print anything it checks 2 MB of RAM, works out how much is there, and then
// hands control to a divMMC ROM that is not part of the image. These tests pin
// the two invariants that took the longest to establish, so a regression in the
// MMU, in Z80N or in the divMMC shows up as a failure here instead of as a
// machine that quietly reboots forever.

#include <cstdio>
#include <string>
#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "peripherals/divmmc.h"

using rainbow::DivMmc;
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

// With the divMMC ROM present the board ROM's RST 0x38 stub writes 0x80 to
// port 0xE3, which maps the module in over the low 8 KiB, and then falls
// through the board ROM's own 0x0048 into the esxDOS code that lives at that
// offset in the divMMC image. 0x1EA0 is the divMMC ROM's "unmap and return"
// entry, so arriving there proves the hand-off happened; without the image the
// same address is zeroed RAM and the machine restarts instead. The budget has
// to cover the board ROM's 2 MB RAM probe, which is what fills most of it, and
// it is measured in instructions rather than T-states because contention
// shifts the second without touching the first.
TEST(boot_real_next_rom_hands_control_to_the_divmmc_rom) {
  Machine machine(MachineType::Next);
  if (!loadNextRom(machine)) return;
  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  if (div == nullptr) return;
  std::string error;
  if (!div->loadRomFile(romPath("nxtmmc.rom"), error)) return;
  machine.expansion().setEnabled("divmmc", true);
  machine.reset();

  CHECK_EQ(countVisits(machine, 0x1EA0, 1, 1000000), 1);
}
