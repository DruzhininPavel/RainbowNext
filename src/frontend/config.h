#pragma once

#include <string>
#include <vector>

namespace mynext::frontend {

// What survives between runs: which peripherals are plugged in and where the
// plugin folder lives. Deliberately a flat `key = value` file so it can be read
// and edited without a tool:
//
//   plugin_dir = /Users/me/Library/Application Support/myNext/peripherals
//   peripheral = kempston
//   peripheral = printer
//
// Anything unrecognised is ignored, so a config written by a newer build still
// opens in an older one.
struct Config {
  std::string pluginDir;
  std::vector<std::string> enabledPeripherals;

  bool load(const std::string& path);
  bool save(const std::string& path) const;
};

} // namespace mynext::frontend
