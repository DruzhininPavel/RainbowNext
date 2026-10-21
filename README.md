# myNext

A from-scratch ZX Spectrum / ZX Spectrum Next emulator written in C++20, built
around a deterministic headless core and a thin SDL3 + Dear ImGui frontend.
Runs on macOS and Linux (including SteamOS / Steam Deck).

```
Status: M0 — 48K Spectrum boots, renders and beeps.
        128K / Next / tape / AY are on the roadmap (docs/ROADMAP.md).
```

## Features (M0)

* **Z80** — full documented and undocumented instruction set: `IXH/IXL/IYH/IYL`,
  `DDCB`, `SLL`, `MEMPTR` (`WZ`), `IM 0/1/2`, `NMI`, `HALT`, EI delay,
  interruptible block instructions.
* **ULA** — port `0xFE` read/write, border, beeper, 8×5 keyboard matrix,
  Fuse-compatible memory contention, floating bus.
* **Timing** — 69 888 T-states per frame at 3.5 MHz, exact frame length asserted
  by tests.
* **Video** — 256×192 screen with correct Spectrum addressing and flashing,
  rendered into a 320×256 ARGB8888 framebuffer with border.
* **Audio** — beeper mixed at 44.1 kHz through an `SDL_AudioStream`.
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
```

A bare path is routed by extension: `.rom`, `.tap`/`.tzx`, `.sna`/`.z80`.
The same files can be dropped onto the window.

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
src/core/       Z80, ULA, keyboard, beeper, Machine — no OS dependencies
src/formats/    SNA / Z80 snapshot loaders, TAP / TZX tape parsers
src/frontend/   SDL3 + Dear ImGui host: app, audio, input, debugger
tests/          unit tests, incl. an end-to-end boot test (no ROM required)
docs/           ARCHITECTURE.md, REFERENCES.md, ROADMAP.md
roms/           user-supplied ROMs (empty by design)
```

## Testing

```sh
ctest --test-dir build --output-on-failure
```

47 tests cover the Z80 core (timings, flags, undocumented opcodes, interrupts),
ULA behaviour, contention, floating bus, frame length, audio rate, both snapshot
formats, and an end-to-end run of a hand-assembled program through the machine.

## License

Not decided yet — see the open questions in [`docs/ROADMAP.md`](docs/ROADMAP.md).
Until then no third-party emulator code is included; other projects are used as
behavioural references only (see [`docs/REFERENCES.md`](docs/REFERENCES.md)).
