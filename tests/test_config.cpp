#include "test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "frontend/config.h"

using rainbow::frontend::Config;

namespace {

// Each test writes its own file, so a failure cannot leak into the next one.
std::string tempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void writeText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc);
  out << text;
}

} // namespace

TEST(config_remembers_the_machine_and_its_roms) {
  const std::string path = tempPath("rainbow_cfg_roundtrip.cfg");
  Config saved;
  saved.machine = "next";
  saved.roms["next"].main = "/games/enNextZX.rom";
  saved.roms["next"].divMmc = "/games/enNxtmmc.rom";
  saved.roms["48"].main = "/games/48.rom";
  CHECK(saved.save(path));

  Config loaded;
  CHECK(loaded.load(path));
  CHECK(loaded.machine == "next");
  CHECK_EQ(loaded.roms.size(), 2u);
  CHECK(loaded.roms["next"].main == "/games/enNextZX.rom");
  CHECK(loaded.roms["next"].divMmc == "/games/enNxtmmc.rom");
  CHECK(loaded.roms["48"].main == "/games/48.rom");
  CHECK(loaded.roms["48"].divMmc.empty());
  std::filesystem::remove(path);
}

// A config written before the ROM keys existed must still open, and must not
// leave half-populated entries behind for machines it never mentioned.
TEST(config_still_reads_a_file_without_rom_keys) {
  const std::string path = tempPath("rainbow_cfg_legacy.cfg");
  writeText(path, "# RainbowNext settings\n"
                  "plugin_dir = /plugins\n"
                  "peripheral = kempston\n"
                  "crt = off\n"
                  "crt_scanlines = 0.5\n");
  Config loaded;
  CHECK(loaded.load(path));
  CHECK(loaded.machine.empty());
  CHECK(loaded.roms.empty());
  CHECK(loaded.pluginDir == "/plugins");
  CHECK_EQ(loaded.enabledPeripherals.size(), 1u);
  CHECK(!loaded.crtEnabled);
  std::filesystem::remove(path);
}

TEST(config_ignores_keys_it_does_not_understand) {
  const std::string path = tempPath("rainbow_cfg_unknown.cfg");
  writeText(path, "machine = plus3\n"
                  "rom.plus3 = /roms/plus3.rom\n"
                  "rom = nobody\n"
                  "rom. = broken\n"
                  "divmmc.next = /roms/nxtmmc.rom\n"
                  "future_option = 7\n");
  Config loaded;
  CHECK(loaded.load(path));
  CHECK(loaded.machine == "plus3");
  CHECK(loaded.roms["plus3"].main == "/roms/plus3.rom");
  // `rom = nobody` has no machine id and `rom. = broken` has an empty one, so
  // neither may invent an entry.
  CHECK_EQ(loaded.roms.count(""), 0u);
  CHECK_EQ(loaded.roms.count("nobody"), 0u);
  CHECK(loaded.roms["next"].divMmc == "/roms/nxtmmc.rom");
  std::filesystem::remove(path);
}

// Loading twice must replace the previous state rather than merge into it: a
// stale machine id would otherwise survive a hand edit of the file.
TEST(config_reload_clears_the_previous_roms) {
  const std::string path = tempPath("rainbow_cfg_reload.cfg");
  writeText(path, "machine = next\nrom.next = /a/next.rom\n");
  Config loaded;
  CHECK(loaded.load(path));
  CHECK_EQ(loaded.roms.size(), 1u);

  writeText(path, "machine = 48\nrom.48 = /a/48.rom\n");
  CHECK(loaded.load(path));
  CHECK(loaded.machine == "48");
  CHECK_EQ(loaded.roms.size(), 1u);
  CHECK_EQ(loaded.roms.count("next"), 0u);
  CHECK(loaded.roms["48"].main == "/a/48.rom");
  std::filesystem::remove(path);
}
