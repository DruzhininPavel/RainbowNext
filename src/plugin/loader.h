#pragma once

#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "core/peripheral.h"
#include "plugin/plugin_api.h"

namespace mynext {

// One plugin loaded from a shared library. It owns the library handle and
// closes it only after the plugin's own destroy callback has run, so the code
// stays mapped for as long as the instance exists.
class PluginPeripheral final : public Peripheral {
public:
  PluginPeripheral(void* library, const mynext_peripheral_v1* api,
                   const HostServices& host);
  ~PluginPeripheral() override;

  PluginPeripheral(const PluginPeripheral&) = delete;
  PluginPeripheral& operator=(const PluginPeripheral&) = delete;

  const PeripheralInfo& info() const override { return info_; }
  void attachHost(const HostServices& host) override;
  void reset() override;
  bool ioRead(u16 port, u32 t, u8& value) override;
  bool ioWrite(u16 port, u8 value, u32 t) override;
  void tick(u32 cycles) override;

  // The path the library was loaded from, for the settings menu.
  const std::string& path() const { return path_; }
  void setPath(std::string path) { path_ = std::move(path); }

private:
  void* library_ = nullptr;
  const mynext_peripheral_v1* api_ = nullptr;
  void* self_ = nullptr;
  PeripheralInfo info_;
  std::string path_;
  // The C mirror of HostServices that the plugin was created with. Its address
  // is handed to the plugin and must outlive it.
  mynext_host_v1* hostC_ = nullptr;
};

// Scans a directory for plugin libraries and turns each one into a Peripheral.
// A plugin that fails to load is reported in `errors` and skipped; one bad file
// never stops the rest.
class PluginLoader {
public:
  struct Result {
    std::vector<std::unique_ptr<Peripheral>> modules;
    std::vector<std::string> errors;
  };

  static Result scan(const std::string& directory, const HostServices& host);

  // The extensions a plugin library may use on this platform, without the dot.
  // macOS accepts both so a library built with a plain `-shared` link line is
  // found whether it was named .dylib or .so.
  static std::vector<std::string> extensions();
};

} // namespace mynext
