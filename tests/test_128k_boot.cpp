#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
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

// Loads a ROM set laid out as one 16 KiB file per bank (roms/128-0.rom, ...).
// Returns false when the set is incomplete, so the test can skip.
bool loadRomSet(Machine& machine, std::initializer_list<const char*> banks) {
  std::vector<u8> image;
  for (const char* name : banks) {
    const std::vector<u8> bank = readFile(std::string(RAINBOW_SOURCE_DIR) + "/roms/" + name);
    if (bank.size() != 0x4000) return false;
    image.insert(image.end(), bank.begin(), bank.end());
  }
  return machine.loadRomImageData(image.data(), image.size());
}

u16 screenCellAddress(int textRow, int column, int pixelLine) {
  const int y = textRow * 8 + pixelLine;
  return u16(0x4000 + ((y & 0xC0) << 5) + ((y & 0x07) << 8) + ((y & 0x38) << 2) + column);
}

// Recovers a text row by matching each cell against the 48K character set. The
// 128K boot ROM does not carry the font, so it is passed in from roms/48.rom.
std::string screenRowText(Machine& machine, const u8* font, int textRow) {
  std::string text;
  for (int column = 0; column < 32; ++column) {
    const u16 address = screenCellAddress(textRow, column, 0);
    int bestChar = '?';
    int bestDistance = 8 * 8 + 1;
    for (int ch = 32; ch < 128; ++ch) {
      int distance = 0;
      for (int line = 0; line < 8; ++line) {
        const u8 glyph = font[(ch - 32) * 8 + line];
        const u8 pixels = machine.memRead(u16(address + (line << 8)), 0);
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

bool screenContains(Machine& machine, const u8* font, const char* needle) {
  for (int row = 0; row < 24; ++row) {
    if (screenRowText(machine, font, row).find(needle) != std::string::npos) return true;
  }
  return false;
}

void holdKeys(Machine& machine, std::initializer_list<std::pair<int, int>> keys, int frames) {
  for (const auto& [row, column] : keys) machine.keyboard().setKey(row, column, true);
  for (int i = 0; i < frames; ++i) machine.runFrame();
  for (const auto& [row, column] : keys) machine.keyboard().setKey(row, column, false);
  for (int i = 0; i < 8; ++i) machine.runFrame();
}

// CAPS SHIFT + 6 is cursor down on the Spectrum keyboard.
void cursorDown(Machine& machine) { holdKeys(machine, {{0, 0}, {4, 4}}, 12); }

} // namespace

// The 128K boots into its menu instead of 48 BASIC, which only works when ROM
// bank selection and the 0x7FFD latch behave.
TEST(boot_real_128k_rom_shows_the_menu) {
  Machine machine(MachineType::Spectrum128);
  if (!loadRomSet(machine, {"128-0.rom", "128-1.rom"})) return;
  const std::vector<u8> fontRom = readFile(std::string(RAINBOW_SOURCE_DIR) + "/roms/48.rom");
  if (fontRom.size() != 0x4000) return;

  for (int i = 0; i < 300; ++i) machine.runFrame();

  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "Tape Loader"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "128 BASIC"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "Calculator"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "Tape Tester"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "1986 Sinclair Research Ltd"));
}

// Picking "48 BASIC" pages ROM 1 in through 0x7FFD and restarts from 0x0000, so
// the 1982 message proves the ROM bank actually changed.
TEST(boot_real_128k_rom_enters_48_basic_through_rom_paging) {
  Machine machine(MachineType::Spectrum128);
  if (!loadRomSet(machine, {"128-0.rom", "128-1.rom"})) return;
  const std::vector<u8> fontRom = readFile(std::string(RAINBOW_SOURCE_DIR) + "/roms/48.rom");
  if (fontRom.size() != 0x4000) return;

  for (int i = 0; i < 300; ++i) machine.runFrame();
  for (int i = 0; i < 3; ++i) cursorDown(machine); // 48 BASIC is the fourth entry
  holdKeys(machine, {{6, 0}}, 12);                 // ENTER
  for (int i = 0; i < 250; ++i) machine.runFrame();

  CHECK_EQ(machine.romBank(), 1);
  CHECK((machine.port7ffd() & 0x10) != 0);
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "1982 Sinclair Research Ltd"));
}

// The +3 uses all four ROM banks and the 0x1FFD latch, and its menu is the
// Amstrad one rather than the Sinclair one.
TEST(boot_real_plus3_rom_shows_the_amstrad_menu) {
  Machine machine(MachineType::Plus3);
  if (!loadRomSet(machine, {"plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom"})) return;
  const std::vector<u8> fontRom = readFile(std::string(RAINBOW_SOURCE_DIR) + "/roms/48.rom");
  if (fontRom.size() != 0x4000) return;

  for (int i = 0; i < 300; ++i) machine.runFrame();

  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "+3 BASIC"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "Loader"));
  CHECK(screenContains(machine, fontRom.data() + 0x3D00, "Amstrad Plc."));
}
