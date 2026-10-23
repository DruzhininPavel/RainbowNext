#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/peripheral.h"

namespace rainbow {

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
  // The reset button: modules keep whatever they hold in RAM.
  void softReset();
  void tick(u32 cycles);

  bool ioRead(u16 port, u32 t, u8& value);
  bool ioWrite(u16 port, u8 value, u32 t);

  // Memory goes to the modules before the machine decodes it, in the same
  // daisy-chain order. `memoryHooked()` is false when no enabled module wants
  // memory, which is the common case and keeps the read path cheap.
  bool memoryHooked() const { return !memory_.empty(); }
  bool memRead(u16 addr, u32 t, bool m1, u8& value);
  bool memWrite(u16 addr, u8 value, u32 t);

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
  void rebuildMemoryList();

  std::vector<Slot> slots_;
  // Enabled modules that intercept memory, in chain order. Rebuilt by
  // `changed()` so a read costs one empty check rather than a virtual call per
  // slot.
  std::vector<Peripheral*> memory_;
  HostServices host_;
  std::function<void()> observer_;
};

} // namespace rainbow
