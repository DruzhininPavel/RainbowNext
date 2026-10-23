#pragma once

#include <map>
#include <string>
#include <vector>

namespace rainbow::frontend {

// The images one machine boots from. `main` is the CPU ROM set; `divMmc` is the
// second image the board hands control to while booting. Only the Next needs a
// companion today -- its RST 38 stub maps the divMMC in and jumps into esxDOS --
// but the slot is per machine so a machine that grows one needs no new key.
struct RomChoice {
  std::string main;
  std::string divMmc;
};

// What survives between runs: the machine to start in, the ROM images chosen
// for each machine, which peripherals are plugged in and where the plugin
// folder lives. Deliberately a flat `key = value` file so it can be read and
// edited without a tool:
//
//   machine = next
//   rom.next = /Users/me/roms/enNextZX.rom
//   divmmc.next = /Users/me/roms/enNxtmmc.rom
//   rom.48 = /Users/me/roms/48.rom
//   plugin_dir = /Users/me/Library/Application Support/RainbowNext/peripherals
//   peripheral = kempston
//   peripheral = printer
//   crt = on
//   crt_scanlines = 0.35
//   crt_mask = 0.45
//
// An empty `rom.<machine>` means "look for a well-known name in roms/", which
// is what a fresh install does. Anything unrecognised is ignored, so a config
// written by a newer build still opens in an older one.
struct Config {
  std::string pluginDir;
  std::vector<std::string> enabledPeripherals;

  // Short machine id ("48", "128", "plus3", "next", ...) to start in.
  std::string machine;
  // ROM images keyed by the same machine id.
  std::map<std::string, RomChoice> roms;

  // CRT filter: off, or a scanline/aperture-grille pass with these strengths.
  bool crtEnabled = true;
  float crtScanlines = 0.35f;
  float crtMask = 0.45f;

  bool load(const std::string& path);
  bool save(const std::string& path) const;
};

} // namespace rainbow::frontend
