# Roadmap

Milestones are ordered so that each one is independently testable and leaves the
emulator in a usable state. Everything is validated by `ctest`; visual checks are
done by loading a ROM or a snapshot.

## M0 — 48K Spectrum

* [x] Z80 interpreter: documented + undocumented opcodes, `IXH/IXL/IYH/IYL`,
      `DDCB`, `SLL`, `MEMPTR` (`WZ`), IM 0/1/2, NMI, HALT, EI delay
* [x] ULA: port `0xFE` read/write, border, beeper, keyboard matrix 8×5
* [x] Contention (Fuse pattern) and floating bus
* [x] Frame timing: 69 888 T-states, 50.08 Hz
* [x] Screen rendering with the classic `((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)` addressing
* [x] Audio sample accumulator (beeper → float samples)
* [x] SNA and Z80 v1/v2/v3 snapshot loading (plain + RLE)
* [x] TAP and TZX tape loading: block model, EAR playback, instant load through
      the ROM loader trap at `0x0556`
* [x] SDL3 + Dear ImGui frontend: window, pacing, menus, drag & drop, debugger overlay
* [x] 62 unit tests including an end-to-end "boot a hand-assembled program" test,
      a `LOAD ""` run from a real TAP, and golden tests that boot the real `roms/48.rom`
* [x] `48.rom` boots to the BASIC prompt: the copyright message is printed and
      `PRINT 42` evaluates to `42` with the `0 OK, 0:1` report
* [ ] `z80test` / `zexall` conformance runs in CI
* [x] TAP/TZX tape loading
* [ ] Golden-frame image hashes

**Exit criteria:** `48.rom` boots to the BASIC prompt and executes BASIC (done);
a multicolour demo renders correctly; `z80test` passes.

## M1 — 128K / +2 / +3 / Pentagon (current)

* [x] `0x7FFD` paging: RAM bank, screen bank, ROM bank, lock bit
* [x] `0x1FFD` (+2A/+3): four CP/M memory maps, ROM bit 2, motor; paging
      is locked only by `0x7FFD` bit 5, which freezes both ports
* [x] ROM bank sets: `128-0.rom`/`128-1.rom`, `plus2-*.rom`, `plus3-0..3.rom`
* [x] AY-3-8912: 3 channels, 17-bit noise LFSR, 16-step envelope, volume
* [x] Stereo output: mono / ABC / ACB mixing, DC-blocking filter
* [x] Frame timings: 69 888 (128K), 70 908 (+2A/+3), 71 680 (Pentagon)
* [x] Contention per paged bank; Pentagon reports no contention
* [x] Real ROM boot verified: 128K menu, ROM paging into 48 BASIC, +3 Amstrad menu
* [ ] Exact 128K contention (`offset` 1 vs 4, `line_times[]`)
* [x] +3 disk controller (µPD765) and `.dsk`/`.edsk`: SPECIFY, SENSE DRIVE,
      SENSE INTERRUPT, RECALIBRATE, SEEK, READ ID, READ DATA/DELETED/TRACK,
      WRITE DATA/DELETED, FORMAT TRACK; standard CPCEMU and extended images
* [ ] Pentagon ULA+ attributes and `0xDFFD`/`0xEFF7` magic ports
* [ ] Real-time tape playback tuning for protected loaders (turbo blocks, custom pilots)

**Exit criteria:** `128-0.rom` boots the menu, ROM paging reaches 48 BASIC, the
+3 ROM set boots its menu, and the AY plays a note through a TAP/TZX loader.

## M2 — ZX Spectrum Next

* [x] Z80N instruction set — SWAPNIB, MIRROR A, TEST n, the five DE shifts,
      MUL D,E, ADD reg,A / ADD reg,nn, PUSH nn, OUTINB, NEXTREG, PIXELDN,
      PIXELAD, SETAE, JP (C), LDIX/LDDX/LDIRX/LDDRX, LDWS and LDPIRX. Enabled
      only on `MachineType::Next`, so the classic machines still see NOPs.
* [x] MMU / NextREG file — eight 8K slots (`0x50`-`0x57`), page 255 showing
      the ROM, ports `0x243B`/`0x253B`, the `0x7FFD` shortcut, alternate ROM
      (`0x8C`), machine type (`0x03`) and the CPU speed register (`0x07`).
      Turbo runs the CPU 2×/4×/8× faster than the ULA instead of speeding up
      the frame.
* [ ] Layer 2 (256×192 and 320×256), ULA+ palettes, Radastan mode
* [ ] 128 sprites (4B/5B), tilemap, Copper, DMA
* [ ] 3× AY with stereo, specdrum, `.nex` snapshot format
* [ ] SD card image access for NextZXOS
* [ ] Video pipeline: ULA/Timex → Layer 2 → sprites → tilemap → compositor → palette

## M3 — Tooling and shipping

* [ ] Debugger: breakpoints, memory editor, disassembler, profiler
* [ ] RZX replay and recording, rewind buffer
* [ ] Save states in a versioned binary format
* [ ] CRT/scanline shaders (SDL_GPU backend)
* [ ] CI releases: macOS universal + SteamOS/Linux AppImage
* [ ] Steam Deck controller profiles, 1280×800 default scaling

## Known gaps

These are deliberate simplifications, tracked here so they are not forgotten:

1. `Machine::contention` uses the simplified pattern without `line_times[]`;
   it can differ from Fuse by a few T-states at the exact border edges.
2. `Ula::reset()` always sets `defaultValue_ = 0xBF`; 128K Issue 3 should use
   `b & 0x10 ? 0xFF : 0xBF`.
3. `ioRead` for ports other than `0xFE` does not return the floating bus on 48K.
4. IM 0 is handled like IM 1 (equivalent on Spectrum, where the bus floats to
   `0xFF` = `RST 38h`).
5. Tape playback is real-time only through the EAR bit; protected loaders that
   depend on exact pilot lengths are best run with instant load enabled.
6. No Kempston mouse, no Kempston joystick.
7. The AY is clocked from the CPU T-state counter rather than from the ULA
   frame counter, so it is deterministic but not cycle-exact against hardware.
8. The µPD765 answers instantly instead of modelling head movement, so a SEEK
   completes in the same frame; the +3 ROM only polls the interrupt line, which
   this satisfies. Writes go straight into the in-memory image and are not
   flushed back to the `.dsk` file.
9. Contention uses one fixed pattern per line instead of a per-line
   `line_times[]` table, so the 128K/+3 windows start a few T-states early.

## Open decisions

* **License.** MIT/Apache-2.0 keeps the option of reading GPL projects as
  reference only. GPL-3.0 would allow lifting code from JNEXT/ZEsarUX/MAME.
* **NextZXOS distribution.** Likely user-supplied SD image only, as JNEXT does.
* **Renderer.** SDL_Renderer is enough for M0-M2; SDL_GPU is the target for
  shaders in M3.
