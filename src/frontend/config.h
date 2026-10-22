#pragma once

#include <string>
#include <vector>

namespace rainbow::frontend {

// What survives between runs: which peripherals are plugged in and where the
// plugin folder lives. Deliberately a flat `key = value` file so it can be read
// and edited without a tool:
//
//   plugin_dir = /Users/me/Library/Application Support/RainbowNext/peripherals
//   peripheral = kempston
//   peripheral = printer
//   crt = on
//   crt_scanlines = 0.35
//   crt_mask = 0.45
//
// Anything unrecognised is ignored, so a config written by a newer build still
// opens in an older one.
struct Config {
  std::string pluginDir;
  std::vector<std::string> enabledPeripherals;

  // CRT filter: off, or a scanline/aperture-grille pass with these strengths.
  bool crtEnabled = true;
  float crtScanlines = 0.35f;
  float crtMask = 0.45f;

  bool load(const std::string& path);
  bool save(const std::string& path) const;
};

} // namespace rainbow::frontend
