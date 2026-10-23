#include "core/expansion.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace rainbow {

void ExpansionBus::add(std::unique_ptr<Peripheral> module, bool enabled) {
  if (!module) return;
  Slot slot;
  slot.module = std::move(module);
  slot.enabled = enabled;
  slot.module->attachHost(host_);
  slot.module->reset();
  if (enabled) slot.module->enable();
  slots_.push_back(std::move(slot));
  changed();
}

bool ExpansionBus::remove(const std::string& id) {
  const auto it = std::find_if(slots_.begin(), slots_.end(),
                               [&](const Slot& s) { return s.module->info().id == id; });
  if (it == slots_.end()) return false;
  if (it->enabled) it->module->disable();
  slots_.erase(it);
  changed();
  return true;
}

void ExpansionBus::clear() {
  for (Slot& slot : slots_) {
    if (slot.enabled) slot.module->disable();
  }
  slots_.clear();
  changed();
}

Peripheral* ExpansionBus::find(const std::string& id) {
  for (Slot& slot : slots_) {
    if (slot.module->info().id == id) return slot.module.get();
  }
  return nullptr;
}

const Peripheral* ExpansionBus::find(const std::string& id) const {
  return const_cast<ExpansionBus*>(this)->find(id);
}

bool ExpansionBus::setEnabled(const std::string& id, bool enabled) {
  for (Slot& slot : slots_) {
    if (slot.module->info().id != id) continue;
    if (slot.enabled == enabled) return true;
    slot.enabled = enabled;
    if (enabled) {
      slot.module->reset();
      slot.module->enable();
    } else {
      slot.module->disable();
    }
    changed();
    return true;
  }
  return false;
}

bool ExpansionBus::enabled(const std::string& id) const {
  for (const Slot& slot : slots_) {
    if (slot.module->info().id == id) return slot.enabled;
  }
  return false;
}

void ExpansionBus::setHost(const HostServices& host) {
  host_ = host;
  for (Slot& slot : slots_) slot.module->attachHost(host_);
}

void ExpansionBus::reset() {
  for (Slot& slot : slots_) slot.module->reset();
}

void ExpansionBus::softReset() {
  for (Slot& slot : slots_) slot.module->softReset();
}

void ExpansionBus::tick(u32 cycles) {
  for (Slot& slot : slots_) {
    if (slot.enabled) slot.module->tick(cycles);
  }
}

bool ExpansionBus::ioRead(u16 port, u32 t, u8& value) {
  for (Slot& slot : slots_) {
    if (!slot.enabled) continue;
    if (slot.module->ioRead(port, t, value)) return true;
  }
  return false;
}

bool ExpansionBus::ioWrite(u16 port, u8 value, u32 t) {
  for (Slot& slot : slots_) {
    if (!slot.enabled) continue;
    if (slot.module->ioWrite(port, value, t)) return true;
  }
  return false;
}

bool ExpansionBus::memRead(u16 addr, u32 t, bool m1, u8& value) {
  for (Peripheral* module : memory_) {
    if (module->memRead(addr, t, m1, value)) return true;
  }
  return false;
}

bool ExpansionBus::memWrite(u16 addr, u8 value, u32 t) {
  for (Peripheral* module : memory_) {
    if (module->memWrite(addr, value, t)) return true;
  }
  return false;
}

const std::vector<u8>* ExpansionBus::rom() const {
  for (const Slot& slot : slots_) {
    if (!slot.enabled) continue;
    if (const std::vector<u8>* image = slot.module->rom()) return image;
  }
  return nullptr;
}

std::string ExpansionBus::enabledList() const {
  std::string result;
  for (const Slot& slot : slots_) {
    if (!slot.enabled) continue;
    if (!result.empty()) result += ',';
    result += slot.module->info().id;
  }
  return result;
}

void ExpansionBus::applyEnabledList(const std::string& list) {
  std::vector<std::string> ids;
  std::string current;
  for (const char c : list) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t') {
      if (!current.empty()) ids.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) ids.push_back(current);

  for (Slot& slot : slots_) {
    const bool want =
        std::find(ids.begin(), ids.end(), slot.module->info().id) != ids.end();
    if (want == slot.enabled) continue;
    slot.enabled = want;
    if (want) {
      slot.module->reset();
      slot.module->enable();
    } else {
      slot.module->disable();
    }
  }
  changed();
}

void ExpansionBus::rebuildMemoryList() {
  memory_.clear();
  for (Slot& slot : slots_) {
    if (slot.enabled && slot.module->interceptsMemory()) {
      memory_.push_back(slot.module.get());
    }
  }
}

void ExpansionBus::changed() {
  rebuildMemoryList();
  if (observer_) observer_();
}

} // namespace rainbow
