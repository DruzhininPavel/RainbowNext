# myNext

A from-scratch ZX Spectrum / ZX Spectrum Next emulator written in C++20, built
around a deterministic headless core and a thin SDL3 + Dear ImGui frontend.
Runs on macOS and Linux (including SteamOS / Steam Deck).

```
Status: M1 — 48K and 128K/+2/+3 boot with AY sound, tape loading, disk images
        and snapshots.
        M2 in progress: the Z80N instruction set, the Next's MMU/NextREG file,
        its palette hardware, Layer 2, sprites and the tilemap are implemented
        (docs/ROADMAP.md).
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
  scanline.
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
  rendered into a 320×256 ARGB8888 framebuffer with border.
* **Audio** — beeper plus AY-3-8912 (3 tone channels, noise, envelope, stereo
  mono/ABC/ACB) mixed at 44.1 kHz through an `SDL_AudioStream`.
* **Tape** — TAP and TZX with a block model, EAR playback and instant load via
  the ROM loader trap at `0x0556`.
* **ROM sets** — banked ROM images (`128-0.rom`…, `plus3-0.rom`…) are assembled
  into a single image, so the machine can page through all four banks.
* **Disk** — +3 µPD765 on ports `0x2FFD`/`0x3FFD` with standard CPCEMU `.dsk`
  and extended `.edsk` images (SPECIFY, SEEK, READ ID, READ/WRITE DATA, FORMAT
  TRACK), driven by the real +3 ROM.
* **Snapshots** — `.sna` (48K/128K) and `.z80` (v1, v2, v3 with RLE).
* **Frontend** — resizable window, integer scaling, speed 1×/2×/4×/8×,
  drag & drop loading, Dear ImGui menus and a debugger overlay.
* **Input** — Spectrum keyboard mapping, Kempston joystick on arrow keys or any
  SDL gamepad (Steam Deck controls work out of the box).

## Build

Requirements: CMake ≥ 3.24, a C++20 compiler, SDL3 ≥ 3.2. If SDL3 is missing it
is downloaded and built automatically (`-DMYNEXT_FETCH_SDL3=OFF` to disable).

### macOS

```sh
brew install cmake sdl3
./scripts/build-macos.sh
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
./build/mynext                      # searches roms/48.rom, roms/48.ROM, 48.rom
./build/mynext roms/48.rom          # explicit ROM
./build/mynext game.tap             # ROM from roms/, tape inserted
./build/mynext --rom 48.rom --tape game.tzx --snapshot save.z80
./build/mynext --rom roms/plus3-0.rom --disk game.dsk
```

A bare path is routed by extension: `.rom`, `.tap`/`.tzx`, `.sna`/`.z80`,
`.dsk`/`.edsk`. The same files can be dropped onto the window.

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
| Reset | `F5` |
| Tape play / stop | `F6` |
| Tape rewind | `F7` |

## Layout

```
src/core/       Z80, ULA, keyboard, beeper, AY, µPD765, Machine — no OS deps
src/formats/    SNA / Z80 snapshots, TAP / TZX tapes, DSK / EDSK disk images
src/frontend/   SDL3 + Dear ImGui host: app, audio, input, debugger
tests/          unit tests, incl. an end-to-end boot test (no ROM required)
docs/           ARCHITECTURE.md, REFERENCES.md, ROADMAP.md
roms/           user-supplied ROMs (empty by design)
```

## Testing

```sh
ctest --test-dir build --output-on-failure
```

263 tests cover the Z80 core (timings, flags, undocumented opcodes,
interrupts), the Z80N extension, the Next MMU, palette, Layer 2, sprites
(including relative and unified sprites and the collision flags), the tilemap,
the Copper and the zxnDMA, ULA
behaviour, contention, floating bus, frame length, audio rate, both snapshot
formats, the AY, 128K/+3 paging and timings, tape blocks, the µPD765 and the DSK
parser, and end-to-end runs of
hand-assembled programs through the machine. Golden tests also boot the real
`roms/128-0.rom` and `roms/plus3-0.rom` sets when those files are present; they
skip themselves on a clean checkout.

## License

Not decided yet — see the open questions in [`docs/ROADMAP.md`](docs/ROADMAP.md).
Until then no third-party emulator code is included; other projects are used as
behavioural references only (see [`docs/REFERENCES.md`](docs/REFERENCES.md)).
