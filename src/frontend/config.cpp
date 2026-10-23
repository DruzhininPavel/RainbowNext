#include "frontend/config.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace rainbow::frontend {
namespace {

std::string trim(const std::string& text) {
  size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  return text.substr(first, last - first);
}

std::optional<float> parseFloat(const std::string& text) {
  try {
    size_t used = 0;
    const float value = std::stof(text, &used);
    if (used == 0) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

// `rom.next` and `divmmc.next` carry a machine id after the dot, so the key is
// split once and the rest is the id.
std::optional<std::string> suffixAfter(const std::string& key, const std::string& prefix) {
  if (key.size() <= prefix.size() + 1) return std::nullopt;
  if (key.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  if (key[prefix.size()] != '.') return std::nullopt;
  return key.substr(prefix.size() + 1);
}

void appendIds(const std::string& list, std::vector<std::string>& out) {
  std::string current;
  for (const char c : list) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) out.push_back(current);
}

} // namespace

bool Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;

  enabledPeripherals.clear();
  roms.clear();
  machine.clear();
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));
    if (key == "plugin_dir") {
      pluginDir = value;
    } else if (key == "machine") {
      machine = value;
    } else if (const auto id = suffixAfter(key, "rom")) {
      if (!id->empty()) roms[*id].main = value;
    } else if (const auto id = suffixAfter(key, "divmmc")) {
      if (!id->empty()) roms[*id].divMmc = value;
    } else if (key == "peripheral") {
      appendIds(value, enabledPeripherals);
    } else if (key == "peripherals") {
      appendIds(value, enabledPeripherals);
    } else if (key == "crt") {
      crtEnabled = value != "off" && value != "0" && value != "false";
    } else if (key == "crt_scanlines") {
      if (const auto parsed = parseFloat(value)) crtScanlines = clamp01(*parsed);
    } else if (key == "crt_mask") {
      if (const auto parsed = parseFloat(value)) crtMask = clamp01(*parsed);
    }
  }
  return true;
}

bool Config::save(const std::string& path) const {
  std::error_code ec;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);

  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << "# RainbowNext settings\n";
  if (!machine.empty()) out << "machine = " << machine << '\n';
  for (const auto& [id, choice] : roms) {
    if (!choice.main.empty()) out << "rom." << id << " = " << choice.main << '\n';
    if (!choice.divMmc.empty()) out << "divmmc." << id << " = " << choice.divMmc << '\n';
  }
  out << "plugin_dir = " << pluginDir << '\n';
  for (const std::string& id : enabledPeripherals) out << "peripheral = " << id << '\n';
  out << "crt = " << (crtEnabled ? "on" : "off") << '\n';
  out << "crt_scanlines = " << crtScanlines << '\n';
  out << "crt_mask = " << crtMask << '\n';
  return out.good();
}

} // namespace rainbow::frontend
