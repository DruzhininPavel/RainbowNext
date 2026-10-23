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
