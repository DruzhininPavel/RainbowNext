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
* [x] `z80test` conformance suite runs as part of the unit tests: `z80doc` and
      `z80docflags` pass 160 of 160, `z80full` and `z80flags` 154 of 160. The
      remaining six are the two `SCF`+`CCF` sequences, whose vectors the suite
      derives from a pseudo-random mix of both instructions, and the four
      `->NOP'` block cases that depend on how an undefined `ED 00` behaves.
      `z80memptr`, which observes the internal WZ register through `BIT n,(HL)`,
      is wired in as a regression guard at 143 of 160: `JR`, `DJNZ`,
      `IN r,(C)`/`OUT (C),r`, `LD (BC),A`/`LD (DE),A` and the `ADD HL,rr`
      family now leave the documented WZ behind, the rest is listed in
      `docs/REFERENCES.md`
* [x] Zilog NMOS `SCF`/`CCF` undocumented-flag rule: F3/F5 come from `A` when
      the previous instruction wrote the flags and are ORed with the old bits
      otherwise. `POP AF` does not count as a flag write, and an intervening
      `DD`/`FD` prefix forgets that a flag write happened
* [x] Synthetic test ROM (`tools/make_test_rom.py` → `tests/data/test48.rom`) so
      the suites run without the copyrighted Spectrum ROM
* [ ] `zexall` conformance run
* [x] TAP/TZX tape loading
* [x] Golden-frame hashes: `tests/test_golden.cpp` renders ten scenes and
      compares an FNV-1a hash of the whole 320×256 surface against a recorded
      value. The scenes build their own memory contents, so no ROM is needed:
      the classic screen with per-cell ink/paper/bright, flash blinking, Timex
      hi-res, ULA+, a Layer 2 gradient, two sprites, the tilemap, LoRes and
      Radastan. Every scene also asserts a few named pixels first, so a hash
      cannot record a renderer that draws the wrong picture

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
* [x] Exact contention: the six-busy, two-idle pattern is anchored to the first
      T-state of the display window and then follows the absolute T-state, so
      the 48K, whose 224-T-state line is a multiple of eight, meets the same
      phase on every line while the 128K, the +3 and the Next, whose lines are
      228 T-states, walk the phase by four and alternate between six and two
      T-states of delay at the first pixel. The +2A/+3 ASIC contends every bank
      at `0xC000`, whereas the 128K and the grey +2 contend only while an odd
      bank is paged there, because the gate array cannot see the even ones.
* [x] +3 disk controller (µPD765) and `.dsk`/`.edsk`: SPECIFY, SENSE DRIVE,
      SENSE INTERRUPT, RECALIBRATE, SEEK, READ ID, READ DATA/DELETED/TRACK,
      WRITE DATA/DELETED, FORMAT TRACK; standard CPCEMU, extended and raw TR-DOS
      images, plus the `MV - CPCEMU +3 Disk Image` header variant; the +3 ROM
      `Loader` boots programs from a `.dsk`
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
* [x] The second Layer 2 palette, selectable for drawing by `0x43` bits 6:4 and
      for display by `0x43` bit 2, and bit 7 of a Layer 2 colour, which the
      blending modes use to lift the pixel above the sprites
* [x] LoRes and Radastan: `0x15` bit 7 replaces the ULA with a 128×96 image
      stretched over the whole 256×192 viewport, two display pixels per dot and
      two rows per image row. LoRes keeps eight bits per dot and adds the `0x6A`
      palette offset to the high nibble only; Radastan takes four bits per dot
      from one palette block, narrowed to `11` & offset(1:0) while ULA+ owns the
      top 64 entries and to the plain high nibble once ULANext takes it back.
      `0x6A` bit 4 XORs the Timex display file select, `0x32`/`0x33` scroll the
      image in half-dot steps, and the clip registers are unimplemented on
      hardware, so LoRes pixels are always opaque
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
      `Peripherals` menu and the choice is written to `rainbow.cfg`
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
* [x] The Next's built-in SD card interface on ports `0xE7` (chip select,
      active low per device) and `0xEB` (data), which is what the board ROM's
      own driver uses. The card itself is `src/core/sdcard.*`: an in-memory
      sector image with the SPI state machine in front of it (`CMD0`-`CMD59`,
      SDHC, block addressing, `CMD18`/`CMD25`/`CMD12`/`CMD23` for the
      multi-block transfers NextZXOS issues). `Machine` owns the card, so it
      survives a machine switch, and `--sd FILE` or a dropped `.img` inserts it
* [x] divMMC built into the Next, as `src/core/divmmc.*`: the `0xE3` control
      latch, the 8K esxDOS ROM (32K image, one 8K page at a time), 16 x 8K RAM
      banks, `CONMEM`/`MAPRAM` mapping, the automap switch on NextReg `0x0A`
      bit 4, the eight entry points from NextReg `0xB8`-`0xBB`, the ROM3 gate
      and the exit-stub unmap after the instruction. This is not a plugin: the
      Next's boot ROM maps it in at `0x0000` and will not continue unless the
      card ROM answers with its `"DV"` signature, so nothing boots without it.
      The card is still the machine's own `SdCard`; divMMC never owns one
* [x] Video pipeline: the ULA → tilemap → Layer 2 → sprite compositor and the
      palette path are in place, plus:
  * Timex SCLD modes on port `0xBF3F`/`0xFF`: standard, `ALTDFILE` (the second
    display file at `+0x2000`), `EXTCOLOUR` (one attribute per character cell
    from the second file) and `HIRES`, whose 512 pixels per line are averaged
    into the 256-pixel framebuffer. Bits 5:3 pick the hi-res colour pair, ink
    being its complement, and the border takes the paper colour
  * ULA+ on ports `0xBF3B`/`0xFF3B`: 64 colours in the low entries of the ULA
    palette, `ink = (FLASH*2 + BRIGHT)*16 + INK` and
    `paper = (FLASH*2 + BRIGHT)*16 + PAPER + 8`, the border taken from entry 8,
    enabled by its own mode register or by NextReg `0x68` bit 3
  * ULANext (NextReg `0x43` bit 0): the attribute becomes a pair of palette
    indices, `ink = attr & 0x42` and `paper = 128 + (attr & ~0x42)`, with the
    mask of `0x42` at `0xFF` turning the whole byte into one ink index and
    `0x4A` supplying the paper and border
  * Blending modes 110 and 111 of `0x15` bits 4:2: Layer 2 is mixed into the
    ULA instead of ordered against it, by addition (110) or by addition minus
    five per channel (111), with `0x68` bits 6:5 choosing which half of the ULA
    layer is the blend colour and which is drawn on top

## M3 — Tooling and shipping

* [x] Debugger: breakpoints, memory editor, disassembler, profiler. `F1` opens
      the window, `F3`/`F4` step and step over, a breakpoint stops the frame
      before the instruction runs, the CPU tab lists the disassembly around PC
      (click a line to toggle a breakpoint), the Memory tab pokes bytes and the
      Profiler tab ranks addresses by how often they were executed
* [x] Save states in a versioned binary format: `RBNSTATE`, a version, the
      machine type, a ROM fingerprint, the body and an FNV-1a checksum. One
      `serialize` template writes and reads every field, so the two directions
      cannot drift apart. `F8`/`F9` quick save and load, `Shift+F8`/`Shift+F9`
      with a file dialog, `--state` on the command line
* [x] Rewind buffer: a ring of states capped by bytes, one recorded every four
      frames while the machine runs. `F10` pauses and steps back through it,
      `View > Rewind buffer` turns the recording off
* [x] RZX replay and recording. The container is the one libspectrum and Fuse
      read: a `RZX!` header, a creator block, a snapshot block and an input
      block whose frames hold the byte every port read returned. A session sits
      on `Machine::ioRead` through an `IoReadHook`, so recording captures what
      the CPU saw rather than what the keyboard held, and a replay substitutes
      the recorded bytes even when the host would answer differently.
      `File > Record RZX` starts one at the current frame boundary and snapshots
      the machine, `File > Stop recording...` names the file, `File > Play RZX...`
      rebuilds the machine from that snapshot and runs the input stream. The
      snapshot uses our own `RBNS` state format, so other emulators cannot open
      a RainbowNext RZX, and blocks are written uncompressed to keep zlib out of
      the dependencies
* [x] CRT/scanline shaders on an SDL_GPU backend. The frontend moved from
      `SDL_Renderer` to `SDL_GPU` (Metal on macOS, Vulkan on Linux) and draws the
      320x256 frame as one triangle through `crt.vert`/`crt.frag`, which adds a
      dark seam between source lines and an aperture grille. Both are strengths
      in `View > CRT strength`, saved in `rainbow.cfg`; the shaders are compiled
      to SPIR-V and MSL by `src/frontend/shaders/build.sh` and checked in, so a
      build needs no shader toolchain
* [x] CI releases: a tagged build of macOS and Linux x86_64, packaged as a
      self-contained zip (SDL3 linked in, `peripherals/` and `roms/` beside the
      executable) and attached to a GitHub release. `v0.1.0` is the first
      release cut this way; `workflow_dispatch` builds the same archives as
      workflow artifacts without publishing them
* [x] macOS universal (arm64 + Intel) and a SteamOS/Linux AppImage. One
      `CMAKE_OSX_ARCHITECTURES` environment variable in the matrix makes the
      macOS job cross-compile both slices, and the Linux job packs the same
      binary into an AppDir that `appimagetool` turns into a single runnable
      file. `v0.1.1` is the first release that ships both
* [x] Steam Deck controller profiles and 1280×800 default scaling. The window
      opens at the largest whole-number zoom of the 320×256 frame that fits the
      screen (3x on the Deck's 1280×800 panel), and a 1280×800 panel or an
      inherited `SteamDeck=1` starts fullscreen. The pad drives Kempston from
      both the stick and the d-pad, and its face and shoulder buttons press
      SPACE, ENTER, CAPS SHIFT and SYMBOL SHIFT through `Keyboard::setPadKey`, a
      layer that sits beside the matrix so releasing the pad cannot clear a key
      the keyboard holds. `Start` summons the toolbar for a pad with no pointer,
      `Back` pauses. See `docs/ARCHITECTURE.md`, "Input and handhelds"
* [x] Persistent ROM settings. `Machine > ROM images...` lists every machine
      with the image it boots from, `Choose...` stores a path and `Forget` drops
      it, both written to `rainbow.cfg` as `rom.<id>` so a fresh start needs no
      `--rom`. Machines that need a second image get a sub-row: the Next has an
      SD card slot (`sd.<id>`), so the card it boots from is loaded together
      with the board ROM instead of being inserted by hand every start. The
      machine itself is stored too (`machine`), and picking a ROM or a card by
      hand writes the same setting, so the window and the manual path cannot
      disagree

## Known gaps

These are deliberate simplifications, tracked here so they are not forgotten:

1. `Machine::contention` derives its window from the border geometry rather
   than from a per-line `line_times[]` table, so the contended window is the
   whole display rectangle and the pattern restarts with it. The phase and the
   per-bank `0xC000` rule match the hardware; what is missing is the Next's
   layer 2 and sprite fetches, which contend on their own schedules.
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
9. The contended window is the display rectangle for every machine, so the
   border lines a ULA fetch steals from the edges are not modelled.
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
    virtual call per memory access. A mass-storage module would also have to be
    plugged in last, because its status port shares `0x1F` with the Kempston
    joystick, and the joystick wins that address.
15. Plugging or unplugging a module resets the machine instead of hot-plugging
    it, and the plugin ABI is version 1, so a module built against a future
    header is refused rather than adapted. Memory hooks were appended to the
    ABI without a version bump, because `struct_size` makes them optional.
16. The SD card image is written in place in memory and never flushed back to
    the image file, so a save from esxDOS survives until the card is ejected or
    the emulator exits. The SPI clock is not modelled: a command is answered on
    the next byte instead of after the few hundred cycles a real card needs, so
    a driver that polls for readiness simply sees it at once.
17. NextZXOS does not boot yet. The divMMC ROM is now in place and the boot
    ROM gets past its card check, so this is no longer the "does the machine
    have the hardware" question it used to be; what is left is the card
    protocol.

    *The gate that was blocking everything.* The boot ROM at `0x1EDB` does
    `OUT (0xE3),A` to map the card ROM over `0x0000` and then checks `0x0004`
    for the `"DV"` signature. `nxtmmc.rom` begins `F3 C3 6A 00 44 56 09 02`,
    which is `"DV"` at offset 4. When the check fails the ROM spins at
    `JR 0x1ED9` forever, and with no divMMC in the core that is exactly what
    happened. Two earlier diagnoses are therefore retired: the reset loop was
    never a crash, and the "third ROM image" was `nxtmmc.rom` being served by
    the overlay.

    *Where it stops now.* With `nxtmmc.rom` loaded the ROM reaches its own
    `0x3BE8` soft reset after about 823k instructions, and the card read it
    issues never completes. The driver it uses is the esxDOS one in the card
    ROM (`0x1F0F`-`0x1F5C`): `0x1F5C` puts `0x51` (`CMD17`) in `A` and calls
    `0x1F15`, which sends `H`, `L`, `D`, `E` as the 32-bit argument and then
    polls `IN A,(0xEB)`. There is no `CMD0`/`CMD8`/`CMD55`/`ACMD41` in that
    path at all, so it is an already-open read, not an initialisation.

    The argument observed is `0x47D5F9AF`, which is out of range and comes back
    as `R1ParameterError` (`0x40`). The registers explain why: at the call
    `HL=0x47D5`, `DE=0xF9AF`, `BC=0x3DD7`, `IX=0x2340`, and the sixteen bytes
    at `IX` are all zero -- the file descriptor the boot ROM is reading through
    was never filled in. So the card is being blamed for a bad address, and the
    fault is upstream in whatever was supposed to open that file. `SdCard`
    itself is correct: driven directly it answers `CMD0` with `0x01`, `CMD8`
    with `01 00 00 01 AA`, `CMD58` with `01 40 80 80 00` and `CMD17@0` with
    `00 FF FE 00 01 02 03 04`.

    The next step is therefore not in `src/core/sdcard.*` but in following how
    the boot ROM populates a descriptor before it maps the card ROM in, starting
    from the `CALL 0x1EAF` at `0x0846` and the `0x07C4` "position the file"
    helper it runs first.

18. A `.nex` loading screen is loaded into memory but never shown: the
    `loadingBar`, `loadingDelay` and `startDelay` fields are parsed and ignored,
    so the program starts at once instead of after an animated bar. The
    expansion-bus disable that V1.3 asks for is likewise parsed but not applied.
19. The framebuffer stays 256 pixels wide, so the 512-pixel Timex hi-res line
    is sampled 2:1 by averaging each pair of hi-res pixels. A dot that is one
    hi-res pixel wide therefore shows as half brightness instead of a full one.
20. Timex modes 3, 4, 5 and 7 have no separate implementation: they reuse the
    second display file like `ALTDFILE` rather than reproducing the `HIRESATTR`
    and `HIRESDOUBLECOL` combinations Fuse models.
21. In blending modes 110 and 111 the half of the ULA layer that is not blended
    is always drawn above the sprites, which is what the modes intend for the
    ULA but not necessarily for the tilemap sharing that layer.

## Open decisions

* **License.** Decided: BSD Zero Clause (0BSD), the most permissive option —
  no conditions at all, only the warranty disclaimer. It keeps the project to
  reading GPL emulators as behavioural references; lifting their code would
  require relicensing, which 0BSD permits but the GPL does not allow in the
  other direction.
* **NextZXOS distribution.** Likely user-supplied SD image only, as JNEXT does.
* **Renderer.** SDL_GPU (Metal/Vulkan) since M3, which is what the CRT filter
  needs. It costs a Vulkan loader on Linux, which SteamOS and every current
  distribution already ship.