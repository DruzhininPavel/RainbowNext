// Fuller Audio Box joystick interface, written as an external plugin.
//
// This is the smallest useful peripheral, and therefore the reference for
// anyone adding one of their own:
//
//   1. Include <rainbow/plugin_api.h> (the only header a plugin needs).
//   2. Fill in a `rainbow_peripheral_v1` descriptor.
//   3. Export `rainbow_plugin_v1()` returning a pointer to it.
//
// Build it however you like - `clang++ -std=c++20 -shared -fPIC fuller.cpp -o
// fuller_joystick.dylib` is enough - and drop the result into the peripherals
// folder next to the emulator. It appears in the Peripherals menu, switched off.
//
// A plugin must never link against the emulator: everything it can do goes
// through the `rainbow_host_v1` table handed to `create()`.

#include "plugin/plugin_api.h"

namespace {

// The Fuller box answers on port 0x7F with the joystick in the low five bits and
// ones everywhere else, so software can tell it apart from a Kempston.
constexpr uint16_t kPort = 0x007F;

struct Fuller {
  const rainbow_host_v1* host = nullptr;
};

void* create(const rainbow_host_v1* host) {
  auto* self = new Fuller;
  self->host = host;
  return self;
}

void destroy(void* instance) { delete static_cast<Fuller*>(instance); }

int io_read(void* instance, uint16_t port, uint32_t, uint8_t* value) {
  if ((port & 0x00FF) != kPort) return 0;
  const auto* self = static_cast<const Fuller*>(instance);
  const uint8_t buttons = self->host->joystick != nullptr ? self->host->joystick(self->host->host) : 0;
  *value = uint8_t(0xE0 | (buttons & 0x1F));
  return 1;  // claimed: the emulator's own decode is skipped for this port
}

const rainbow_peripheral_v1 kFuller = {
    /* abi_version */ RAINBOW_PLUGIN_ABI_VERSION,
    /* struct_size */ sizeof(rainbow_peripheral_v1),
    /* id          */ "fuller",
    /* name        */ "Fuller Box Joystick",
    /* version     */ "1.0",
    /* author      */ "RainbowNext",
    /* description */ "Joystick interface on port 0x7F. Reference plugin; the source "
                      "lives in plugins/fuller/fuller.cpp.",
    /* flags       */ 0,
    /* create      */ create,
    /* destroy     */ destroy,
    /* reset       */ nullptr,
    /* io_read     */ io_read,
    /* io_write    */ nullptr,
    /* tick        */ nullptr,
};

}  // namespace

extern "C" RAINBOW_PLUGIN_EXPORT const rainbow_peripheral_v1* rainbow_plugin_v1(void) {
  return &kFuller;
}
