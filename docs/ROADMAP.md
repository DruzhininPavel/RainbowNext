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
* [x] Palette hardware — eight 256-entry palettes of 9-bit RGB333 colours
      (`0x40`-`0x44`), 8-bit and two-byte 9-bit write protocols, auto-increment,
      the classic Spectrum colours as the soft-reset defaults. The ULA, the
      border and the paper half of the palette now read through it, so changing
      entry `16+n` recolours the border.
* [x] Layer 2 — `0x69` bit 7 and the `0x123B` port, the 256×192, 320×256 and
      640×256×4 resolutions of `0x70`, the active bank (`0x12`), the 9-bit X and
      8-bit Y scroll (`0x16`, `0x17`, `0x71`), the global transparency index
      (`0x14`) and the layer priority order of `0x15`. Layer 2 is composited
      over the ULA per pixel while the ULA still owns the border.
* [x] Layer 2 clip window (`0x18`, four writes: X1, X2, Y1, Y2, applied to the
      layer coordinates before compositing).
* [x] Sprites — 128 sprites of 16×16 with four or five attribute bytes
      (`0x34`-`0x39`, `0x75`-`0x79`, ports `0x303B`/`0x57`/`0x5B`), a separate
      16K pattern RAM, 8-bit and 4-bit patterns, palette offsets, mirror,
      rotate, 1×-8× scaling, per-sprite transparency (`0x4B`), the sprite clip
      window (`0x19`, reset through `0x1C`) and the whole `0x15` control byte:
      enable, over-border, clipping in over-border mode, sprite-0-on-top and
      the layer priority order.
* [x] Tilemap (layer 3) — `0x6B` control byte (enable, 40/80 columns,
      no-attributes, second palette, text mode, 512 tiles, force over the ULA),
      the 2-byte map entries of `0x6E`/`0x6F` in bank 5 or 8K bank 7, 4-bit
      tiles of 32 bytes or monochrome text tiles of 8, the default attribute of
      `0x6C`, mirror/rotate, the 9-bit X and 8-bit Y scroll (`0x2F`-`0x31`), the
      clip window of `0x1B` and the transparency index of `0x4C`. The layer
      reaches into the border and mixes with the ULA per pixel.
* [x] Relative and unified (big) sprites — byte 4 of `0x39`/`0x79` with its top
      two bits at `01` makes a sprite hang off the last anchor above it in the
      list: the anchor's position, pattern size, and optionally its pattern
      (`PO`) and palette offset (`PR`). Bit 5 of an anchor's byte 4 groups its
      relatives into a single big sprite that follows the anchor's mirror,
      rotate and scale. The status port `0x303B` reports a collision between
      two sprites (bit 0) and the per-scanline sprite budget running out
      (bit 1), and clears both as it is read.
* [ ] The second Layer 2 palette and its priority bit, ULA+ palettes, Radastan
      mode
* [x] The Copper of registers `0x60`-`0x63`: 2K of program memory addressed by
      `0x61` (low byte) and bits 2:0 of `0x62` (high three bits), the `WAIT`
      and `MOVE` instructions, `0x0000` as a no-op and `0xFFFF` as the halt,
      the four modes of `0x62` (stop, restart, continue, restart every frame),
      a repeated mode that only moves the write index, and a compositor that
      samples the register file per scanline so a write shows up on its line.
* [x] zxnDMA on port `0x6B`, with `0x0B` as the Zilog compatibility port that
      moves one byte more than the programmed length: the WR0-WR6 register
      groups with their parameter lists, port A and B each memory or IO and
      incrementing, decrementing or fixed, `LOAD`, `ENABLE`, `CONTINUE`, the
      prescalar that paces a byte every `32 * prescalar` master clocks so the
      DMA can feed the DAC, auto restart at the end of a block, and the masked
      read sequence that reports the status byte and the live pointers
* [x] TurboSound Next on port `0xFFFD`: three AY-3-8912 chips where bit 7 of
      the register select byte chooses a chip and bits 6 and 5 enable its left
      and right output, NextReg `0x08` bit 1 enables chips 1 and 2 and bit 5
      swaps the middle and the right chip (ABC or ACB), and NextReg `0x09`
      bits 7:5 force a chip to mono
* [x] Four 8-bit DACs on the SpecDrum and SoundDrive ports (`0x1F`, `0x0F`,
      `0x4F`, `0x5F`, `0xDF`, `0xB3` and their mirrors), with DAC A and B on the
      left, C and D on the right, and the `0x2C`/`0x2D`/`0x2E` NextReg mirrors
* [x] The expansion bus: `Peripheral` as the module contract, `ExpansionBus`
      as an ordered daisy chain that hands each I/O access to the enabled
      modules in plug-in order and lets a module claim it, `HostServices` as the
      way back into the machine without recursing through the bus, and an
      observer that re-reads a cartridge's ROM. Four built-in modules ship:
      Kempston joystick, Kempston mouse, ZX Printer with PBM export, and a ROM
      cartridge that maps into `0x0000`. Modules are switched on and off in the
      `Peripherals` menu and the choice is written to `mynext.cfg`
* [x] Out-of-tree modules as shared libraries: the plain C ABI of
      `src/plugin/plugin_api.h`, a loader that scans a `peripherals/` folder,
      skips a broken library instead of failing, and a worked example
      (`plugins/fuller`)
* [x] `.nex` program format: V1.0-V1.3, banks read in the loader's order,
      palette, Layer 2 and ULA loading screens, entry bank, and the register
      state `nexload` leaves behind. Loading one switches the machine to the Next
* [x] Memory in the `Peripheral` contract: `interceptsMemory()`, `memRead` with
      an M1 flag, `memWrite`, and `Bus::memFetch` so a module can map itself in
      on an opcode fetch and shadow `0x0000`/`0x2000`
* [x] DivMMC: an 8K or 32K esxDOS ROM, sixteen 8K RAM banks, `mapram`, the
      automap on `0x0000`/`0x0008`/`0x0038`/`0x0066`/`0x04C6`/`0x0562`, and an
      SPI SD card on an in-memory sector image (`CMD0`-`CMD59`, SDHC, block
      addressing). Loaded from the `Peripherals` menu
* [ ] SD card image access for NextZXOS, sharing the `SdCard` state machine
* [ ] Video pipeline: Timex modes, ULA+ palettes, blending modes 6 and 7 of the
      `0x15` priority byte (the ULA → tilemap → Layer 2 → sprite compositor and
      the palette path are in place)

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
6. Kempston joystick and Kempston mouse exist only as expansion modules, so
   they answer only while they are plugged in and enabled. A module cannot be
   added or removed without resetting the machine.
7. The AY is clocked from the CPU T-state counter rather than from the ULA
   frame counter, so it is deterministic but not cycle-exact against hardware.
8. The µPD765 answers instantly instead of modelling head movement, so a SEEK
   completes in the same frame; the +3 ROM only polls the interrupt line, which
   this satisfies. Writes go straight into the in-memory image and are not
   flushed back to the `.dsk` file.
9. Contention uses one fixed pattern per line instead of a per-line
   `line_times[]` table, so the 128K/+3 windows start a few T-states early.
10. The Copper's horizontal `WAIT` position is decoded but does not delay the
    instruction: the compositor samples the register file once per scanline, so
    a Copper write lands at the start of its line rather than at its column.
    Likewise a `MOVE` costs no T-states in the frame budget.
11. A DMA block with no prescalar is copied inside the `ENABLE` write, so the
    CPU loses no time to it; on hardware the transfer does stop the CPU and can
    swallow a maskable interrupt. The prescalar is counted from the CPU clock
    rather than from a fixed 28 MHz, so a turbo machine paces its transfers
    faster. The variable cycle lengths of ports A and B are stored but do not
    slow anything down.
12. NextReg `0x06` bits 1:0 (the audio chip mode: YM, AY, ZXN-8950 or hold all
    AY in reset) is decoded but not applied, so all three chips always behave
    as AY-3-8912.
13. NextReg `0x08` bit 3, which enables the four DACs, is not checked: they are
    always live, so a program that leaves them off still hears its writes. The
    DAC outputs are not low-pass filtered, so a fast stream of samples aliases
    instead of rolling off.
14. A module can claim memory as well as ports, but only inside
    `0x0000-0xFFFF` with no notion of contention, and the bus offers every
    access to the modules in plug-in order, so a module in the chain costs a
    virtual call per memory access. The DivMMC is plugged in last because its
    status port shares `0x1F` with the Kempston joystick, and the joystick wins
    that address.
15. Plugging or unplugging a module resets the machine instead of hot-plugging
    it, and the plugin ABI is version 1, so a module built against a future
    header is refused rather than adapted. Memory hooks were appended to the
    ABI without a version bump, because `struct_size` makes them optional.
16. The DivMMC's SD card is written in place in memory and never flushed back to
    the image file, so a save from esxDOS survives until the card is ejected or
    the emulator exits. The SPI clock is not modelled: a command is answered on
    the next byte instead of after the few hundred cycles a real card needs, so
    a driver that polls for readiness simply sees it at once.
17. The DivMMC does not decode NextReg `0xB8`-`0xBB`, the programmable automap
    entry points, and its status port at `0x1F` is an approximation (card detect
    and busy only); the exact bit layout of a real board was not available.
18. A `.nex` loading screen is loaded into memory but never shown: the
    `loadingBar`, `loadingDelay` and `startDelay` fields are parsed and ignored,
    so the program starts at once instead of after an animated bar. The
    expansion-bus disable that V1.3 asks for is likewise parsed but not applied.

## Open decisions

* **License.** MIT/Apache-2.0 keeps the option of reading GPL projects as
  reference only. GPL-3.0 would allow lifting code from JNEXT/ZEsarUX/MAME.
* **NextZXOS distribution.** Likely user-supplied SD image only, as JNEXT does.
* **Renderer.** SDL_Renderer is enough for M0-M2; SDL_GPU is the target for
  shaders in M3.