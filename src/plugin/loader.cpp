#include "plugin/loader.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace rainbow {

namespace {

constexpr size_t kHostSize = sizeof(rainbow_host_v1);

// The fields that were there from ABI 1. A plugin is accepted when it declares
// at least this much, so a newer header with appended fields stays compatible
// with an older plugin and the other way round.
constexpr uint32_t kPeripheralMinimum =
    uint32_t(offsetof(rainbow_peripheral_v1, tick) + sizeof(void (*)(void*, uint32_t)));
constexpr uint32_t kPeripheralMemory =
    uint32_t(offsetof(rainbow_peripheral_v1, mem_write) +
             sizeof(int (*)(void*, uint16_t, uint8_t, uint32_t)));
constexpr uint32_t kPeripheralFetch =
    uint32_t(offsetof(rainbow_peripheral_v1, mem_fetch) +
             sizeof(int (*)(void*, uint16_t, uint32_t, uint8_t*)));

void* openLibrary(const std::string& path, std::string& error) {
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (handle == nullptr) error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
  return reinterpret_cast<void*>(handle);
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* reason = dlerror();
    error = reason != nullptr ? reason : "dlopen failed";
  }
  return handle;
#endif
}

void closeLibrary(void* handle) {
  if (handle == nullptr) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

void* findSymbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

bool hasExtension(const std::string& path, const std::string& ext) {
  if (path.size() <= ext.size()) return false;
  std::string tail = path.substr(path.size() - ext.size());
  std::transform(tail.begin(), tail.end(), tail.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return tail == ext;
}

} // namespace

PluginPeripheral::PluginPeripheral(void* library, const rainbow_peripheral_v1* api,
                                   const HostServices& host)
    : library_(library), api_(api) {
  hostC_ = static_cast<rainbow_host_v1*>(std::calloc(1, kHostSize));
  info_.id = api->id != nullptr ? api->id : "plugin";
  info_.name = api->name != nullptr ? api->name : info_.id;
  info_.version = api->version != nullptr ? api->version : "1.0";
  info_.author = api->author != nullptr ? api->author : "";
  info_.description = api->description != nullptr ? api->description : "";
  info_.nextOnly = (api->flags & RAINBOW_PERIPHERAL_NEXT_ONLY) != 0;
  attachHost(host);
  if (api->create != nullptr) self_ = api->create(hostC_);
}

PluginPeripheral::~PluginPeripheral() {
  if (api_ != nullptr && api_->destroy != nullptr && self_ != nullptr) {
    api_->destroy(self_);
  }
  std::free(hostC_);
  closeLibrary(library_);
}

void PluginPeripheral::attachHost(const HostServices& host) {
  if (hostC_ == nullptr) return;
  hostC_->struct_size = uint32_t(kHostSize);
  hostC_->host = host.host;
  hostC_->io_read = host.ioRead;
  hostC_->io_write = host.ioWrite;
  hostC_->mem_read = host.memRead;
  hostC_->mem_write = host.memWrite;
  hostC_->next_reg_read = host.nextRegRead;
  hostC_->next_reg_write = host.nextRegWrite;
  hostC_->joystick = host.joystick;
}

void PluginPeripheral::reset() {
  if (api_ != nullptr && api_->reset != nullptr && self_ != nullptr) api_->reset(self_);
}

bool PluginPeripheral::ioRead(u16 port, u32 t, u8& value) {
  if (api_ == nullptr || api_->io_read == nullptr) return false;
  u8 byte = value;
  if (api_->io_read(self_, port, t, &byte) == 0) return false;
  value = byte;
  return true;
}

bool PluginPeripheral::ioWrite(u16 port, u8 value, u32 t) {
  if (api_ == nullptr || api_->io_write == nullptr) return false;
  return api_->io_write(self_, port, value, t) != 0;
}

bool PluginPeripheral::interceptsMemory() const {
  if (api_ == nullptr || api_->struct_size < kPeripheralMemory) return false;
  return api_->mem_read != nullptr || api_->mem_write != nullptr ||
         (api_->struct_size >= kPeripheralFetch && api_->mem_fetch != nullptr);
}

bool PluginPeripheral::memRead(u16 addr, u32 t, bool m1, u8& value) {
  if (!interceptsMemory()) return false;
  int (*read)(void*, uint16_t, uint32_t, uint8_t*) = api_->mem_read;
  if (m1 && api_->struct_size >= kPeripheralFetch && api_->mem_fetch != nullptr) {
    read = api_->mem_fetch;
  }
  if (read == nullptr) return false;
  u8 byte = value;
  if (read(self_, addr, t, &byte) == 0) return false;
  value = byte;
  return true;
}

bool PluginPeripheral::memWrite(u16 addr, u8 value, u32 t) {
  if (!interceptsMemory() || api_->mem_write == nullptr) return false;
  return api_->mem_write(self_, addr, value, t) != 0;
}

void PluginPeripheral::tick(u32 cycles) {
  if (api_ != nullptr && api_->tick != nullptr) api_->tick(self_, cycles);
}

PluginLoader::Result PluginLoader::scan(const std::string& directory,
                                        const HostServices& host) {
  Result result;
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    result.errors.push_back("no plugin folder at " + directory);
    return result;
  }

  const std::vector<std::string> suffixes = extensions();
  std::vector<std::string> candidates;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string path = entry.path().string();
    for (const std::string& suffix : suffixes) {
      if (hasExtension(path, "." + suffix)) {
        candidates.push_back(path);
        break;
      }
    }
  }
  // Directory order is unspecified; a stable order keeps the menu and the
  // enabled list the same from run to run.
  std::sort(candidates.begin(), candidates.end());

  for (const std::string& path : candidates) {
    std::string error;
    void* handle = openLibrary(path, error);
    if (handle == nullptr) {
      result.errors.push_back(path + ": " + error);
      continue;
    }
    const auto entry =
        reinterpret_cast<rainbow_plugin_entry_fn>(findSymbol(handle, RAINBOW_PLUGIN_ENTRY));
    if (entry == nullptr) {
      result.errors.push_back(path + ": no " RAINBOW_PLUGIN_ENTRY " symbol");
      closeLibrary(handle);
      continue;
    }
    const rainbow_peripheral_v1* api = entry();
    if (api == nullptr) {
      result.errors.push_back(path + ": the entry point returned null");
      closeLibrary(handle);
      continue;
    }
    if (api->abi_version != RAINBOW_PLUGIN_ABI_VERSION ||
        api->struct_size < kPeripheralMinimum) {
      result.errors.push_back(path + ": built for ABI " + std::to_string(api->abi_version) +
                              ", this emulator speaks " +
                              std::to_string(RAINBOW_PLUGIN_ABI_VERSION));
      closeLibrary(handle);
      continue;
    }
    if (api->id == nullptr || api->id[0] == '\0') {
      result.errors.push_back(path + ": the plugin has no id");
      closeLibrary(handle);
      continue;
    }
    auto module = std::make_unique<PluginPeripheral>(handle, api, host);
    module->setPath(path);
    result.modules.push_back(std::move(module));
  }
  return result;
}

std::vector<std::string> PluginLoader::extensions() {
#if defined(_WIN32)
  return {"dll"};
#elif defined(__APPLE__)
  return {"dylib", "so"};
#else
  return {"so"};
#endif
}

} // namespace rainbow
