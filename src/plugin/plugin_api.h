/*
 * myNext peripheral plugin ABI.
 *
 * A plugin is a shared library that exports one symbol, `mynext_plugin_v1`,
 * returning a pointer to a table of plain C function pointers. Nothing in this
 * header is C++, so a plugin does not link against the emulator and does not
 * care which compiler or standard library built it.
 *
 *   #include "plugin/plugin_api.h"
 *
 *   static const mynext_peripheral_v1 kFuller = { ... };
 *   extern "C" MYNEXT_PLUGIN_EXPORT
 *   const mynext_peripheral_v1* mynext_plugin_v1(void) { return &kFuller; }
 *
 * See plugins/fuller/fuller.cpp for a complete example.
 */
#ifndef MYNEXT_PLUGIN_API_H
#define MYNEXT_PLUGIN_API_H

#include <stdint.h>

#ifdef _WIN32
#  define MYNEXT_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define MYNEXT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a struct below changes shape. The loader refuses a plugin
 * that was built against a different number instead of calling into a table it
 * does not understand. */
#define MYNEXT_PLUGIN_ABI_VERSION 1u

/* The symbol a plugin must export. */
#define MYNEXT_PLUGIN_ENTRY "mynext_plugin_v1"

/* info.flags bits. */
#define MYNEXT_PERIPHERAL_NEXT_ONLY 0x0001u

/*
 * What the machine offers a plugin. The callbacks reach the machine's own
 * devices only; they never come back through the expansion bus, so a plugin
 * cannot recurse into itself.
 */
typedef struct mynext_host_v1 {
  uint32_t struct_size; /* sizeof(mynext_host_v1), for forward compatibility */
  void* host;           /* opaque machine handle, passed back to every call */

  uint8_t (*io_read)(void* host, uint16_t port, uint32_t t);
  void (*io_write)(void* host, uint16_t port, uint8_t value, uint32_t t);
  uint8_t (*mem_read)(void* host, uint16_t addr, uint32_t t);
  void (*mem_write)(void* host, uint16_t addr, uint8_t value, uint32_t t);
  uint8_t (*next_reg_read)(void* host, uint8_t reg);
  void (*next_reg_write)(void* host, uint8_t reg, uint8_t value);
  /* Kempston-packed joystick state: bit 0 right, 1 left, 2 down, 3 up, 4 fire. */
  uint8_t (*joystick)(void* host);
} mynext_host_v1;

/*
 * One peripheral. Every callback except `create` may be null.
 *
 * io_read / io_write return non-zero when the module claims the access; zero
 * passes it on to the next module and finally to the machine. A module that
 * only wants to watch a port (a printer, say) does its work and returns zero.
 */
typedef struct mynext_peripheral_v1 {
  uint32_t abi_version; /* must equal MYNEXT_PLUGIN_ABI_VERSION */
  uint32_t struct_size; /* sizeof(mynext_peripheral_v1) */

  const char* id;    /* stable, stored in the config file, e.g. "fuller" */
  const char* name;  /* shown in the settings menu */
  const char* version;
  const char* author;
  const char* description;
  uint32_t flags; /* MYNEXT_PERIPHERAL_* */

  void* (*create)(const mynext_host_v1* host);
  void (*destroy)(void* self);
  void (*reset)(void* self);
  int (*io_read)(void* self, uint16_t port, uint32_t t, uint8_t* value);
  int (*io_write)(void* self, uint16_t port, uint8_t value, uint32_t t);
  void (*tick)(void* self, uint32_t cycles);
} mynext_peripheral_v1;

typedef const mynext_peripheral_v1* (*mynext_plugin_entry_fn)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MYNEXT_PLUGIN_API_H */
