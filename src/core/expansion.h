#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/peripheral.h"

namespace mynext {

// The edge connector. Modules are offered the bus in the order they were added,
// which is the order of a real daisy chain: the first module that claims an
// access wins, and whatever is left reaches the machine's own devices.
//
// The bus owns its modules. It knows nothing about Machine, so a test can build
// one, plug a module in and drive it without a CPU or a ROM.
class ExpansionBus {
public:
  struct Slot {
    std::unique_ptr<Peripheral> module;
    bool enabled = false;
  };

  // Plugging a module in also resets it, and the observer fires so the machine
  // can re-read whatever the modules contribute (a cartridge ROM, mostly).
  void add(std::unique_ptr<Peripheral> module, bool enabled = false);
  bool remove(const std::string& id);
  void clear();

  Peripheral* find(const std::string& id);
  const Peripheral* find(const std::string& id) const;

  bool setEnabled(const std::string& id, bool enabled);
  bool enabled(const std::string& id) const;

  // The host callbacks every module sees. The address of the stored struct is
  // stable, so a module may keep a pointer to it.
  void setHost(const HostServices& host);
  const HostServices& host() const { return host_; }

  // Fired after anything that changes what the modules contribute.
  void setObserver(std::function<void()> observer) { observer_ = std::move(observer); }

  void reset();
  void tick(u32 cycles);

  bool ioRead(u16 port, u32 t, u8& value);
  bool ioWrite(u16 port, u8 value, u32 t);

  // The ROM of the first enabled cartridge, or null.
  const std::vector<u8>* rom() const;

  const std::vector<Slot>& slots() const { return slots_; }
  size_t size() const { return slots_.size(); }

  // "kempston,printer" -- the form the config file stores. Ids keep the order
  // they were added in, so a round trip is stable.
  std::string enabledList() const;
  void applyEnabledList(const std::string& list);

private:
  void changed();

  std::vector<Slot> slots_;
  HostServices host_;
  std::function<void()> observer_;
};

} // namespace mynext
