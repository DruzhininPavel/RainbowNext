# Architecture

myNext is split into a **deterministic core** and a **thin host frontend**, the
same shape used by RustZX (`core` / `host`), JNEXT (`core` / `gui`) and
Tom Harte's CLK (processor + clocked components).

```mermaid
flowchart TD
    subgraph core["mynext_core (headless, no OS deps)"]
        Z80["Z80 CPU<br/>cycle-accurate, per-instruction"]
        BUS["Bus interface<br/>memRead/memWrite/ioRead/ioWrite<br/>contention/tick/now"]
        ULA["ULA<br/>video, border, port 0xFE,<br/>contention, floating bus"]
        KBD["Keyboard matrix 8x5"]
        BEEP["Beeper"]
        MACH["Machine<br/>memory map, paging, IRQ,<br/>framebuffer, audio accumulator"]
        AY["AY-3-8912<br/>tone, noise, envelope, stereo"]
        FDC["µPD765<br/>+3 disk controller,<br/>two ports, non-DMA"]
        FMT["Formats<br/>SNA, Z80, TAP, TZX, DSK"]
    end

    subgraph host["Frontend (SDL3 + Dear ImGui)"]
        APP["App<br/>window, frame pacing, menus"]
        AUD["Audio<br/>SDL_AudioStream, F32"]
        INP["Input<br/>keyboard mapping, Kempston, gamepads"]
        DBG["Debugger overlay"]
    end

    Z80 --> BUS
    BUS --> MACH
    MACH --> ULA
    MACH --> KBD
    MACH --> BEEP
    MACH --> AY
    MACH --> FDC
    MACH --> FMT
    APP --> MACH
    APP --> AUD
    APP --> INP
    APP --> DBG
```

## Design rules

1. **The core never touches the OS.** No `SDL_*`, no `<filesystem>` I/O in the
   hot path, no globals, no threads. Anything platform specific lives in
   `src/frontend`. This is what makes `ctest` runnable in CI without a display
   and what makes headless golden-frame tests possible later.

2. **One master clock.** The CPU is stepped instruction by instruction and asks
   the bus for contention wait states; the ULA derives video, border and audio
   timing from the same T-state counter. Nothing is scheduled by frame or by
   scanline alone — the frame is an *emergent* property of
   `tstatesPerFrame()`.

3. **`Bus` is the only CPU↔machine contract.** `contention(addr, t)` returning
   extra T-states keeps the CPU free of Spectrum-specific knowledge and lets
   tests plug in a flat bus (`FlatBus` in `tests/test_z80.cpp`).

4. **Determinism.** Given the same ROM, the same input sequence and the same
   start state, the emulator produces the same T-state count and the same
   framebuffer. `tests/test_boot.cpp::boot_is_deterministic` guards this.

5. **Block instructions are interruptible.** `LDIR`, `CPIR`, `INIR`, `OTIR`
   execute one iteration per `step()` and rewind `PC` by two bytes, which is
   exactly what the hardware does and lets an interrupt land between
   iterations.

## Timings

Timings come from Fuse's `libspectrum/timings.c` (`base_timings`), used as a
behavioural reference only:

| Machine | CPU Hz | AY Hz | T-states/line | T-states/frame |
|---|---|---|---|---|
| 48K | 3 500 000 | — | 224 | 69 888 |
| 128K / +2 | 3 546 900 | 1 773 400 | 228 | 70 908 |
| +2A / +3 | 3 546 900 | 1 773 400 | 228 | 70 908 |
| Pentagon | 3 584 000 | 1 792 000 | 224 | 70 908 |

Contention follows the Fuse pattern `{5,4,3,2,1,0,0,6}` indexed by
`(x + 7) & 7` and only applies to `0x4000-0x7FFF` and `0xC000-0xFFFF` while the
contended bank is paged into the screen window.

## Data flow of one frame

```
runFrame()
  └─ while (frames_ < target)
       stepInstruction()
         ├─ Z80::step()  ──fetch/read/write──►  Machine::memRead/memWrite
         │                     └─ contention ──►  Machine::contention
         │                     └─ tick(n)    ──►  Machine::tick
         │                                            ├─ audioAccum_ += step * sampleRate
         │                                            ├─ borderLines_[line] = border
         │                                            └─ frame end: renderFrame()
         └─ (interrupt raised at frame start, held for interruptLength T-states)
```

## Memory map

```
0x0000-0x3FFF  ROM bank 0 or 1 (bit 4 of port 0x7FFD)
0x4000-0x7FFF  RAM bank 5 (always the contended screen bank)
0x8000-0xBFFF  RAM bank 2
0xC000-0xFFFF  RAM bank 0-7 (bits 0-2 of port 0x7FFD)
```

## Disk

The +3's µPD765 lives in `src/core/upd765.*` and talks to the CPU through the
two ports the real machine uses: `0x2FFD` (main status on read, command on
write) and `0x3FFD` (data). It is a four-phase state machine — idle, command,
execution, result — and the ROM drives it byte by byte, polling `RQM`/`DIO`
between transfers, so no timing model is needed to satisfy it.

The image is a plain `formats::DiskImage` decoded from a CPCEMU `.dsk` or an
extended `.edsk` by `src/formats/dsk.cpp`. Both containers carry a Track-Info
block per track with an eight-byte entry per sector (C, H, R, N, ST1, ST2,
length), which is exactly the CHRN tuple the controller reports in READ ID and
searches for in READ/WRITE DATA. Sectors are stored with `128 << n` bytes so a
read can hand out a fixed-size block regardless of how the image was written.

`Upd765` borrows the image instead of owning it: `Machine` holds the
`DiskImage` and re-points the controller after a load or a reset, which keeps
the controller free of I/O and lets tests construct one on the stack.

## Video pipeline

One frame is composed pixel by pixel, scanline by scanline.
`renderFrame()` walks the 320x256 framebuffer row by row. For each row it maps
the framebuffer row back to its raster line with `rasterLineFor()`, hands that
line to the Copper, and then samples the layer registers -- the priority table
of `0x15`, the ULA blanking bit of `0x68`, the tilemap view, the Layer 2 view
and the recorded border byte. Sampling per scanline is what lets the Copper
change a colour, a scroll offset or a priority partway down the frame: the
change is visible from the line it was made on.

The row itself is then resolved column by column: the ULA pixel (decoded into
`ulaRow` by `renderUlaRow()` when the row is a screen row), the tilemap pixel
and the Layer 2 pixel are all looked up for that coordinate and stacked by
`layerOrder()`, which decodes the priority table of register `0x15`.

The tilemap shares the ULA's display layer. When both cover the same pixel, the
tile's own "ULA on top" bit picks the winner; a force bit in `0x6B` overrides
it, and `0x68` bit 0 combines the two with a bitwise AND instead. Register
`0x68` bit 7 blanks the ULA without touching the tilemap, so it is applied per
pixel rather than by skipping the row.

While stacking, the composer records an `occluders_` mask: a pixel is set when
an opaque layer sits above the sprites in the priority order. `renderSpriteRow()`
then makes a second pass over that one row -- sprites may reach into the border
and may sit above or below the other layers -- and skips the pixels the mask
marks as hidden. This keeps the expensive sprite scan out of the layer loop and
gives sprite-0-on-top, over-border drawing and the priority table for free. The
`0x303B` collision map is one scanline wide for the same reason: it is filled
while the row is drawn and cleared for the next one.

Every colour goes through `NextPalette`, which maps a palette index to ARGB.
The ULA and the border read the ULA palettes (register `0x43` bit 1 selects the
second one), Layer 2 reads one of its two palettes (bit 2), the sprites read
theirs (bit 3) and the tilemap picks its palette with bit 4 of `0x6B`. Bits 6:4
of `0x43` only choose where palette *writes* land.

## Sprite resolution

`Sprites::resolve()` walks the sprite list once, from sprite 0 upwards, and
turns the raw attribute bytes into a `SpriteState` per sprite: position,
pattern number, palette offset, pattern size, mirror, rotate and scale. A
sprite whose byte 4 has its top two bits at `01` is *relative*: instead of
carrying a position it carries signed offsets from the anchor that precedes it,
and it borrows the anchor's pattern size and optionally adds the anchor's
pattern (`PO`) and palette offset (`PR`). An anchor whose byte 4 has bit 5 set
is *unified*: its relatives are parts of one big sprite, so they also copy the
anchor's mirror, rotate and scale, and their offsets are scaled and turned with
the anchor. Rendering then works purely from the resolved states, which keeps
the relative logic in one place.

`renderSpriteRow()` also maintains the two flags of the status port `0x303B`.
Collisions come from a bit-per-pixel `spriteDrawn_` map, one scanline wide and
cleared as the row starts: a sprite that writes over a pixel another sprite
already drew raises the collision flag. The budget check counts how many sprites
cover the row being drawn; the hundredth sprite on a line is the last one that
fits, and anything past it is dropped from the frame and raises the overflow
flag. Both flags clear themselves when the port is read, so a program can poll
them per frame without a separate reset.

## Copper

The Copper lives in `src/core/copper.*` and is the machine's own small
coprocessor rather than part of the CPU: it has 2K of program memory, addressed
as 1024 two-byte instructions, and a ten-bit program counter that wraps back to
the start. `Machine::renderFrame()` calls `Copper::runToLine()` with the raster
line it is about to draw, and the Copper runs until it blocks on a `WAIT` for a
later line, calling back into `writeNextReg()` for every `MOVE`. That callback
is the whole interface, which keeps the Copper independent of the machine and
lets tests drive it with a recording lambda.

The instruction word shares one bit between the two opcodes: `1hhhhhhv vvvvvvvv`
is a `WAIT` for line `v` at horizontal position `h*8`, and `0rrrrrrr vvvvvvvv`
is a `MOVE` of `v` into Next register `r`. `0x0000` is a no-op, and `0xFFFF`
waits for a line past the end of the frame, which is how a program halts. The
run loop is bounded to one pass over the program so a program without waits
cannot stall the frame.

Register `0x61` holds the low byte of the write index and bits 2:0 of `0x62`
its top three bits; bits 7:6 of `0x62` are the mode -- stop, restart, continue,
or restart every frame. A control write whose mode bits match the previous
value only moves the write index, so a program can be uploaded while it is
running. `0x63` takes a word as two writes, high byte then low byte, and
commits it only when the second one arrives, so a half-written instruction is
never executed.

The horizontal position of a `WAIT` is decoded and kept, but the compositor
samples the register file once per scanline, so it does not delay the
instruction past the start of its line. That is the one deliberate
simplification in the model; it is what the pixel-accurate version would need
mid-line register sampling for.

## DMA

`src/core/dma.*` is the zxnDMA, one channel that can copy a block between two
addresses where either side may be memory or an IO port. Like the Copper it
does not know about `Machine`: the constructor of the machine hands it two
`std::function` callbacks that read and write a byte through `memRead`,
`memWrite`, `ioRead` or `ioWrite`, so a test can drive it against a flat buffer
and count the accesses.

The CPU writes the control bytes into `0x6B` (or `0x0B` for the Zilog
compatibility port, which moves one byte more than the programmed length) and
the device decodes them in the order the datasheet expects: a base register
byte first, then the parameter bytes that its bits requested. `WR0` carries the
direction and the requests for the start address of port A and the length,
`WR1` and `WR2` describe port A and its timing, `WR4` describes port B, `WR5`
holds auto restart, and `WR6` is a command. The parameter list is reset by every
base byte, which is what makes an interrupted sequence harmless.

`LOAD` copies the programmed start addresses into the live pointers and the
length into the counter; `ENABLE` starts the transfer. Without a prescalar the
whole block is copied inside the `ENABLE` write, because the DMA is faster than
the CPU and the block always finishes before the next instruction. With a
prescalar the transfer becomes asynchronous: `Machine::tick()` calls
`ZxnDma::advance()` with the elapsed master clocks, and a byte moves every
`32 * prescalar` of them, which is the 875 kHz of the datasheet divided down.
That is the mode a program uses to stream samples to the DAC while the CPU
keeps running.

At the end of a block the end-of-block flag of the status byte is cleared, and
either auto restart rewinds the pointers and the counter for the next block or
the channel disables itself. Reading `0x6B` returns a sequence of seven bytes --
status, counter, port A, port B -- masked by the read mask, which `WR6` can
narrow so a program polls only the status byte. The read index wraps, so a
program that reads past the end of its mask starts over instead of reading
garbage.

## Frame buffer

`Machine::framebuffer()` is `320x256` ARGB8888: a 32-pixel border around a
`256x192` screen. The frontend uploads it as an SDL streaming texture, so a
shader-based CRT filter can be dropped in later without touching the core.

## Adding a machine

1. Add an entry to `MachineType` and a row to `timingsFor()`.
2. Extend `Machine::memRead` / `memWrite` / `ioRead` / `ioWrite` for new paging
   or peripherals.
3. Add a unit test next to `tests/test_ula.cpp` that asserts the T-state count
   of one frame.
