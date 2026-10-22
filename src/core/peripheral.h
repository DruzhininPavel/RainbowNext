#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow {

// What a module declares about itself. The frontend shows these strings in the
// settings menu, and the bus stores `id` in the config file, so an id must stay
// stable across versions: it is the only handle a saved configuration has.
struct PeripheralInfo {
  std::string id;
  std::string name;
  std::string version = "1.0";
  std::string author;
  std::string description;
  // Ports the module decodes, for the settings menu. A low byte means the
  // module only looks at A7-A0, a full address means it decodes all 16 lines.
  std::vector<u16> ports;
  // The module needs the Next's register file and will be refused elsewhere.
  bool nextOnly = false;
};

// The machine hands this to every module. The callbacks reach the machine's own
// devices only -- they do not come back through the expansion bus -- so a
// module can never recurse into itself by reading a port it claims.
struct HostServices {
  void* host = nullptr;
  u8 (*ioRead)(void* host, u16 port, u32 t) = nullptr;
  void (*ioWrite)(void* host, u16 port, u8 value, u32 t) = nullptr;
  u8 (*memRead)(void* host, u16 addr, u32 t) = nullptr;
  void (*memWrite)(void* host, u16 addr, u8 value, u32 t) = nullptr;
  u8 (*nextRegRead)(void* host, u8 reg) = nullptr;
  void (*nextRegWrite)(void* host, u8 reg, u8 value) = nullptr;
  // The frontend's joystick state, already packed the Kempston way (bit 0
  // right, 1 left, 2 down, 3 up, 4 fire), so an interface module does not have
  // to grow an input path of its own.
  u8 (*joystick)(void* host) = nullptr;

  bool valid() const { return host != nullptr; }
  u8 readPort(u16 port, u32 t) const { return ioRead ? ioRead(host, port, t) : 0xFF; }
  void writePort(u16 port, u8 value, u32 t) const {
    if (ioWrite) ioWrite(host, port, value, t);
  }
  u8 readMem(u16 addr, u32 t) const { return memRead ? memRead(host, addr, t) : 0xFF; }
  void writeMem(u16 addr, u8 value, u32 t) const {
    if (memWrite) memWrite(host, addr, value, t);
  }
  u8 joystickState() const { return joystick ? joystick(host) : 0; }
};

// A device on the edge connector. The bus offers every I/O access to every
// enabled module in the order they were plugged in; the first module that
// returns true owns the access. Returning false passes it to the next module
// and finally to the machine itself, which is what lets a module watch a port
// (a printer seeing the ULA's writes) without claiming it.
class Peripheral {
public:
  virtual ~Peripheral() = default;

  virtual const PeripheralInfo& info() const = 0;

  // The host services, re-supplied whenever the bus they point at changes.
  virtual void attachHost(const HostServices& host) { (void)host; }

  // Switched on, switched off, or the machine reset. `reset()` is also called
  // once when the module is first added.
  virtual void reset() {}
  virtual void enable() {}
  virtual void disable() {}

  virtual bool ioRead(u16 port, u32 t, u8& value) {
    (void)port;
    (void)t;
    (void)value;
    return false;
  }
  virtual bool ioWrite(u16 port, u8 value, u32 t) {
    (void)port;
    (void)value;
    (void)t;
    return false;
  }

  // Memory is offered before the machine decodes it, so a module can shadow
  // ROM or RAM -- which is what a DivMMC's automap does when it lays its own
  // ROM over 0x0000. Same rule as the ports: true means the module answered.
  //
  // Every read of every instruction goes through here, so a module that wants
  // memory must say so in `interceptsMemory()`; the bus then keeps it on a
  // short list and leaves the hot path alone when nothing is plugged in.
  // `m1` is set for an opcode fetch. An automapping device needs it: it maps
  // its ROM in when the CPU fetches an opcode at a known hook address, and it
  // must not react to an operand read of the same address.
  virtual bool interceptsMemory() const { return false; }
  virtual bool memRead(u16 addr, u32 t, bool m1, u8& value) {
    (void)addr;
    (void)t;
    (void)m1;
    (void)value;
    return false;
  }
  virtual bool memWrite(u16 addr, u8 value, u32 t) {
    (void)addr;
    (void)value;
    (void)t;
    return false;
  }

  // A cartridge supplies ROM. The machine maps the first enabled one at 0x0000
  // and ignores its internal ROM while that module is present.
  virtual const std::vector<u8>* rom() const { return nullptr; }

  virtual void tick(u32 cycles) { (void)cycles; }

  // Modules that take an image advertise a filter here and the frontend shows a
  // "Load image..." entry for them. Null means the module takes no file.
  virtual const char* fileFilter() const { return nullptr; }
  virtual bool loadFile(const std::string& path, std::string& error) {
    (void)path;
    error = "this peripheral takes no image";
    return false;
  }
};

} // namespace rainbow
