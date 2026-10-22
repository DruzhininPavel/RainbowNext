# RainbowNext

A from-scratch ZX Spectrum / ZX Spectrum Next emulator written in C++20, built
around a deterministic headless core and a thin SDL3 + Dear ImGui frontend.
Runs on macOS and Linux (including SteamOS / Steam Deck).

```
Status: v0.1.3 — M1 and M2 done: 48K and 128K/+2/+3 boot with AY sound, tape
        loading, disk images and snapshots, plus the Z80N instruction set, the
        Next's MMU/NextREG file, its palette hardware, Layer 2, sprites, the
        tilemap and the video pipeline (Timex, ULA+, ULANext, blending).
        M3 is done: save states, a rewind buffer, RZX replay and recording, the
        debugger and the CRT filter on an SDL_GPU renderer (docs/ROADMAP.md).
```

## Features

* **Z80** — full documented and undocumented instruction set: `IXH/IXL/IYH/IYL`,
  `DDCB`, `SLL`, `MEMPTR` (`WZ`), `IM 0/1/2`, `NMI`, `HALT`, EI delay,
  interruptible block instructions.
* **Z80N** — the Next extension: `SWAPNIB`, `MIRROR`, `TEST`, the `DE` barrel
  shifts, `MUL`, `ADD` to the wide registers, `PUSH nn`, `OUTINB`, `NEXTREG`,
  `PIXELDN`/`PIXELAD`/`SETAE`, `JP (C)`, `LDIX`/`LDDX`/`LDIRX`/`LDDRX`,
  `LDWS`, `LDPIRX`. Off on the classic machines, where the opcodes stay NOPs.
* **Next MMU** — eight 8K slots in registers `0x50`-`0x57`, page 255 for the
  ROM, ports `0x243B`/`0x253B`, the classic `0x7FFD` shortcut, alternate ROM,
  and a CPU speed register that really does run the CPU 2×/4×/8× faster.
* **Next palette and Layer 2** — eight 256-entry palettes of 9-bit RGB333
  colours written through `0x40`-`0x44`, and a Layer 2 with 256×192, 320×256 and
  640×256×4 modes, hardware scroll, a clip window, a transparency index and the
  layer priority order of register `0x15`, composited over the ULA every
  scanline. Bit 7 of a colour lifts its pixel above the sprites in the blending
  modes.
* **Next sprites** — all 128 sprites of 16×16 with 8-bit or 4-bit patterns from
  a separate 16K pattern RAM, palette offsets, mirror, rotate, 1×-8× scaling,
  per-sprite transparency, a clip window, over-border drawing and the full
  layer priority order — including sprite 0 on top when the program asks for it.
  Relative and unified (big) sprites work off an anchor sprite, and the status
  port `0x303B` reports collisions and a scanline that ran out of sprite budget.
* **Next tilemap** — the layer 3 of `0x6B`, with 40- and 80-column maps, 4-bit
  tiles or monochrome text tiles, 512-tile mode, a default attribute, mirror,
  rotate, hardware scroll, a clip window, a transparency index, and per-tile
  control over whether it sits over or under the ULA.
* **Copper** — the 2K program memory of registers `0x60`-`0x63` with its `WAIT`
  and `MOVE` instructions, the four control modes of `0x62`, and a compositor
  that samples the register file once per scanline so a Copper write lands on
  the line it was made for.
* **zxnDMA** — the single DMA channel of port `0x6B`, with `0x0B` as the Zilog
  compatibility port: WR0-WR6 register groups and their parameter lists, both
  ports memory or IO and incrementing, decrementing or fixed, `LOAD` and
  `ENABLE`, `CONTINUE`, auto restart at the end of a block, and the prescalar
  that paces a byte every `32 * prescalar` clocks so a block can be streamed
  out as sampled audio while the CPU keeps running.
* **ULA** — port `0xFE` read/write, border, beeper, 8×5 keyboard matrix,
  Fuse-compatible memory contention, floating bus.
* **Machines** — 48K, 128K, +2, +3 and Pentagon, each with its own timings
  (69 888 / 70 908 / 71 680 T-states per frame at 3.5 MHz).
* **Paging** — `0x7FFD` RAM/screen/ROM bank + lock, `0x1FFD` CP/M special paging
  and the +3 ROM bit, contention following the paged bank.
* **Video** — 256×192 screen with correct Spectrum addressing and flashing,
  rendered into a 320×256 ARGB8888 framebuffer with border. On the Next the ULA
  also answers the Timex SCLD port (`ALTDFILE`, `EXTCOLOUR` and 512-pixel
  `HIRES`), the ULA+ palettes of ports `0xBF3B`/`0xFF3B` and ULANext, and the
  compositor implements the blending priority modes 110 and 111 of register
  `0x15`, where Layer 2 is added to the ULA or added and darkened.
* **Audio** — beeper plus a TurboSound Next of three AY-3-8912 (3 tone
  channels, noise, envelope, stereo mono/ABC/ACB per chip, `0xFFFD` chip
  select with per-chip left/right enables) and four 8-bit DACs on the SpecDrum
  and SoundDrive ports, mixed at 44.1 kHz through an `SDL_AudioStream`.
* **Tape** — TAP and TZX with a block model, EAR playback and instant load via
  the ROM loader trap at `0x0556`.
* **ROM sets** — banked ROM images (`128-0.rom`…, `plus3-0.rom`…) are assembled
  into a single image, so the machine can page through all four banks.
* **Disk** — +3 µPD765 on ports `0x2FFD`/`0x3FFD` with standard CPCEMU `.dsk`
  and extended `.edsk` images (SPECIFY, SEEK, READ ID, READ/WRITE DATA, FORMAT
  TRACK), driven by the real +3 ROM.
* **Snapshots** — `.sna` (48K/128K) and `.z80` (v1, v2, v3 with RLE).
* **RZX** — record and replay the RZX container: the input block holds the value
  every port read returned, so a replay reproduces the run regardless of what
  the keyboard is doing. See [RZX replay](#rzx-replay).
* **Next programs** — `.nex` (V1.0–V1.3): the RAM banks in the loader's own
  order, the optional palette, Layer 2 and ULA loading screens, the entry bank
  and the loader's register state. Loading one switches the machine to the Next.
* **Modular peripherals** — an expansion bus in the spirit of the edge
  connector. A Kempston joystick, a Kempston mouse, a ZX Printer (with PBM
  export), a ROM cartridge and a DivMMC ship with the emulator; each is a module
  that can be switched on and off in `Peripherals`, and the choice is remembered
  in `rainbow.cfg`. Further modules are shared libraries dropped into a
  `peripherals/` folder next to the executable — the emulator finds them at
  startup and needs no rebuild (see [Writing a peripheral](#writing-a-peripheral)).
* **DivMMC and SD card** — the mass-storage interface esxDOS runs on: an 8K (or
  32K) DivMMC ROM, sixteen 8K RAM banks, the automap on the hook addresses, and
  an SPI SD card that talks to a plain sector image. Load `esxdos.rom` and a
  card image from `Peripherals`; both are yours to supply.
* **Frontend** — resizable window, integer scaling, speed 1×/2×/4×/8×,
  drag & drop loading, Dear ImGui menus and a debugger with breakpoints, a
  disassembly listing, a memory editor and a profiler.
* **Renderer** — SDL_GPU (Metal on macOS, Vulkan on Linux) with a CRT pass:
  a dark seam between scanlines and an aperture grille, both adjustable in
  `View > CRT strength`. Turn it off for a pixel-exact picture.
* **Input** — Spectrum keyboard mapping, Kempston joystick on arrow keys or any
  SDL gamepad (Steam Deck controls work out of the box), Kempston mouse when the
  module is plugged in.

## Download

Every tagged release carries a ready-to-run archive per platform on the
[releases page](../../releases): `rainbow-<version>-macos-universal.zip` and
`rainbow-<version>-linux-x86_64.zip`. The macOS archive is universal, so it runs
on Apple Silicon and on Intel Macs alike. Unpack it anywhere and start the
emulator from that folder:

```sh
./rainbow
```

Linux also gets a single-file `rainbow-<version>-linux-x86_64.AppImage`, which
needs no unpacking at all:

```sh
chmod +x rainbow-*.AppImage && ./rainbow-*.AppImage
```

On macOS `run-rainbow.command` does the same thing from Finder; if Gatekeeper
refuses the downloaded binary, clear the flag once with
`xattr -d com.apple.quarantine rainbow`. SDL3 is linked into the binary, so
nothing has to be installed; the Linux builds still need an X11 or Wayland
session, a Vulkan driver and the usual audio libraries. The ROMs are **not** included — see
[`roms/README.md`](roms/README.md).

## Build

Requirements: CMake ≥ 3.24, a C++20 compiler, SDL3 ≥ 3.2. If SDL3 is missing it
is downloaded and built automatically (`-DRAINBOW_FETCH_SDL3=OFF` to disable).
Pass `-DRAINBOW_STATIC_SDL3=ON` to build a static SDL3 even when a system copy is
installed, which is what the release archives do.

The picture is drawn through SDL_GPU, so the machine needs Metal (macOS) or
Vulkan (Linux). The CRT shaders are checked in as compiled SPIR-V and MSL in
`src/frontend/shaders/crt_shaders.h`; after editing `crt.vert` or `crt.frag`,
regenerate it with `src/frontend/shaders/build.sh` (needs `glslang` and
`spirv-cross`, `brew install glslang spirv-cross`).

### macOS

```sh
brew install cmake sdl3
./scripts/build-macos.sh
```

The release build is universal. Homebrew only ships one architecture, so the
release workflow asks for a static SDL3 built from source and lets CMake compile
both slices:

```sh
CMAKE_OSX_ARCHITECTURES="arm64;x86_64" cmake -B build-universal \
  -DCMAKE_BUILD_TYPE=Release -DRAINBOW_STATIC_SDL3=ON
cmake --build build-universal
```

### SteamOS / Linux

```sh
sudo steamos-readonly disable          # SteamOS only
sudo pacman -S --needed base-devel cmake sdl3
./scripts/build-steamos.sh
```

### Manual

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

ROM images are not bundled — see [`roms/README.md`](roms/README.md).

```sh
./build/rainbow                      # searches roms/ next to the binary, then 48.rom
./build/rainbow roms/48.rom          # explicit ROM
./build/rainbow game.tap             # ROM from roms/, tape inserted
./build/rainbow --rom 48.rom --tape game.tzx --snapshot save.z80
./build/rainbow --rom roms/plus3-0.rom --disk game.dsk
./build/rainbow --rom roms/48.rom --sd nextzxos.img   # DivMMC SD card
./build/rainbow --state quick.rbn      # carry on from a saved state
```

A bare path is routed by extension: `.rom`, `.tap`/`.tzx`, `.sna`/`.z80`,
`.nex`, `.dsk`/`.edsk`, `.img`, `.rbn`. The same files can be dropped onto the
window.

Tape playback is real-time through the EAR bit, so `LOAD ""` works as on real
hardware. `Tape > Instant load` (on by default) short-circuits the ROM loader for
instant loading; turn it off to hear and debug the actual tape signal.

### Keys

| Spectrum | Host |
|---|---|
| `CAPS SHIFT` | Left Shift |
| `SYMBOL SHIFT` | Right Shift |
| `ENTER` | Return |
| `SPACE` | Space |
| letters / digits | same key |
| Kempston | arrow keys, or gamepad stick + A/B |
| Debugger | `F1` |
| Pause | `F2` |
| Step / step over | `F3` / `F4` |
| Reset | `F5` |
| Tape play / stop | `F6` |
| Tape rewind | `F7` |
| Quick save state | `F8` |
| Quick load state | `F9` |
| Save / load state as... | `Shift+F8` / `Shift+F9` |
| Rewind one step | `F10` |

### Save states

`File > Save state...` writes everything the machine is — CPU registers, RAM,
paging, NextRegs, palette, sprites, the border and the framebuffer — into one
`.rbn` file. `F8` and `F9` do the same without a dialog, using
`quick.rbn` next to `rainbow.cfg`, which is what you want in the middle of a
game. A state names the machine it was written for and is fingerprinted against
the ROM, so loading one switches to that machine and refuses to run on a
different ROM.

The file is `RBNSTATE`, a version, the machine type, a ROM fingerprint, the
body, and an FNV-1a checksum over all of it. A damaged, truncated or
newer-versioned file is rejected instead of loaded as garbage; bumping the
version in `machine.cpp` is how the format is allowed to change.

Tapes, disks and the SD card image are **not** embedded — they are large,
user-supplied files. A state restores where the tape head was and the frontend
keeps the media attached, so re-insert a tape before loading a state that needs
one.

`F10` walks back through a ring of states the emulator records while it runs
(one every four frames, up to 256 MB, so roughly a minute of 48K play). It
pauses the machine, and pressing it again steps further back. `View > Rewind
buffer` switches the recording off if the memory is wanted for something else.

## RZX replay

`File > Record RZX` starts recording from the current frame, and the toolbar
shows `REC` while it runs. `File > Stop recording...` asks for a name and writes
a `.rzx` file; `File > Discard recording` throws it away.

`File > Play RZX...` loads one back, or pass it on the command line:
`rainbow --rzx demo.rzx`. The machine is rebuilt from the snapshot stored in the
file and then fed the recorded input, so the run does not depend on what the
keyboard is doing. The toolbar shows `REPLAY`, and the emulator pauses when the
last frame is done.

What is recorded is the value each port read returned, not the state of the
keys — that is what the format stores and what makes a replay exact. A file
carries a full save state, so a 48K recording is about 2.4 MB no matter how many
frames it holds, and the blocks are left uncompressed. The snapshot uses
RainbowNext's own state format, which means these files are for this emulator
only: Fuse and other RZX players will refuse them.

## Layout

```
src/core/          Z80, ULA, AY, DACs, µPD765, SD card, Machine — no OS deps
src/core/          peripheral.h and expansion.h: the bus and the module contract
src/peripherals/   built-in modules: kempston, kempston mouse, printer,
                   cartridge, divmmc (SPI SD card shared from src/core/sdcard)
src/plugin/        the C ABI and the loader for out-of-tree modules
plugins/           example plugin: a Fuller Audio Box joystick interface
src/formats/       SNA / Z80 / NEX images, TAP / TZX tapes, DSK / EDSK disks
src/frontend/      SDL3 + Dear ImGui host: app, audio, input, debugger, config
tests/             unit tests, incl. an end-to-end boot test (no ROM required)
docs/              ARCHITECTURE.md, REFERENCES.md, ROADMAP.md
roms/              user-supplied ROMs (empty by design)
```

## Writing a peripheral

A module is one class deriving from `rainbow::Peripheral`
(`src/core/peripheral.h`). Implement `info()` and whatever you need of
`ioRead`/`ioWrite`/`rom`/`tick`/`loadFile`, and remember that `ioRead`/`ioWrite`
return **true only when the module owns the access** — return false and the
access travels on to the next module and then to the machine, which is how a
module can watch a port without claiming it.

A module that needs memory instead of (or as well as) ports overrides
`interceptsMemory()` to return true, and then `memRead`/`memWrite`. `memRead`
gets an `m1` flag that is true only for an opcode fetch, so a module can map
itself in when the CPU *executes* at an address without reacting to an ordinary
read of the same address — that is how the DivMMC automaps. Claiming an address
and storing nothing is allowed; the boolean means "mine", not "changed".

Two ways to ship it:

1. **Built-in** — put the source in `src/peripherals/`, add it to `rainbow_core`
   in `CMakeLists.txt` and create it in `Machine::installPeripherals()`. Use
   this while you develop, since you get the debugger and the tests for free.
2. **Plugin** — implement the plain C ABI of `src/plugin/plugin_api.h` and
   export a `rainbow_plugin_v1` symbol. `plugins/fuller/fuller.cpp` is a complete
   worked example; `rainbow_fuller` in `CMakeLists.txt` shows the link line. The
   result is a `.dylib`/`.so`/`.dll` you drop into the `peripherals/` folder
   (next to the executable, or wherever `plugin_dir` in `rainbow.cfg` points) and
   switch on in `Peripherals`. The ABI in `rainbow_peripheral_v1` has a
   `struct_size`, so fields are only ever appended: a plugin built against an
   older header keeps working, and the loader reads the optional memory hooks
   only when the struct is long enough to have them.

The settings file lives at `~/Library/Application Support/RainbowNext/RainbowNext/rainbow.cfg`
on macOS and in the SDL pref path on Linux, and is a flat `key = value` file:

```
plugin_dir = /path/to/peripherals
peripheral = kempston
peripheral = printer
```

## Testing

```sh
ctest --test-dir build --output-on-failure
```

414 tests cover the Z80 core (timings, flags, undocumented opcodes,
interrupts), the disassembler (base, CB, ED, indexed and Z80N forms, lengths and
flow), the debugger's breakpoints and profiler, save states (round trips of the
CPU, memory, paging and peripherals, and the refusals for a damaged, truncated,
foreign or differently-ROM'd file), the rewind buffer, the Z80N extension, the
Next MMU, palette, Layer 2, the Timex modes and the ULA+/ULANext attribute
paths, the blending priority modes,
sprites (including relative and unified sprites and the collision flags), the
tilemap, the Copper and the zxnDMA, ULA behaviour, contention, floating bus,
frame length, audio rate, both snapshot formats, the NEX parser and its
application to the machine, the AY, the TurboSound chip select and stereo bits,
the SpecDrum and SoundDrive DAC ports, 128K/+3 paging and timings, tape blocks,
the µPD765 and the DSK parser, the expansion bus with its modules and plugin
loader, the DivMMC and its SPI SD card (single- and multi-block reads and
writes), the settings file, and end-to-end runs of hand-assembled programs
through the machine. Golden tests also boot the real
`roms/128-0.rom` and `roms/plus3-0.rom` sets when those files are present; they
skip themselves on a clean checkout.

## License

[BSD Zero Clause (0BSD)](LICENSE) — do whatever you want with it: use it, change
it, sell it, ship it closed, no attribution needed. The software comes with no
warranty of any kind.

The archive ships SDL3 (zlib) and Dear ImGui (MIT) linked in; their notices are
reproduced in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) and travel with
the release archives. No emulator code was copied from other projects — Fuse,
ZEsarUX, JNEXT and MAME were read as behavioural references only
([`docs/REFERENCES.md`](docs/REFERENCES.md)).

**No ROMs, no NextZXOS and no games are distributed**, here or in the release
archives: those are still under copyright by Amstrad plc and SpecNext Ltd. See
[`roms/README.md`](roms/README.md).
