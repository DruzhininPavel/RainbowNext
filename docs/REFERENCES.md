# References

myNext is written from scratch. The projects below were read to learn *what*
the hardware does and *how* other emulators structure the work. **No GPL code
was copied**; where a file is GPL, only the observable behaviour (timing tables,
port semantics, memory layouts) was reimplemented.

## Timing and ULA behaviour (read as specification)

| Source | License | What was taken from it |
|---|---|---|
| [Fuse](https://github.com/FuseEmulator/fuse-emulator-svn) — `libspectrum/timings.c`, `fuse/spectrum.c`, `fuse/ula.c`, `fuse/display.h` | GPL-2.0 | `base_timings` table for 48K/128K/+2A/+3/Pentagon; contention pattern `{5,4,3,2,1,0,0,6}`; floating-bus column rules; port 0xFE read/write semantics and `ula_default_value` |
| [ZEsarUX](https://github.com/chernandezba/zesarux) | GPL-3.0 | Confirmation of contention ranges and border timing edge cases |
| [raxoft/z80test](https://github.com/raxoft/z80test) | MIT | Flag-behaviour expectations; planned as an external conformance suite |
| [raxoft/zexall](https://github.com/raxoft/zexall) | — | Planned full Z80 conformance run |

## Architecture and structure (read as design reference)

| Source | License | What was taken from it |
|---|---|---|
| [JNEXT](https://github.com/pxdigger/jnext) | GPL-3.0 | Core/frontend split, accuracy model (CPU per instruction, video per scanline, audio per sample), "measure T-states/sec, not FPS", NextREG/Copper/DMA decomposition |
| [RustZX](https://github.com/rustzx/rustzx) | MIT | `core` / `host` crate split, snapshot format handling |
| [CLK / Tom Harte](https://github.com/TomHarte/CLK) | GPL-3.0 | Clocked-component decomposition, `Clockable`-style interfaces |
| [MAME `spectrum.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/sinclair/spectrum.cpp) | GPL-2.0 | Device decomposition, +3 disk controller notes |
| [JSSpeccy 3](https://github.com/gasman/jsspeccy3) | GPL-3.0 | Browser-oriented accuracy/perf trade-offs, test ROM harness ideas |

## ZX Spectrum Next

| Source | License | What was taken from it |
|---|---|---|
| [SpecNext extended Z80 instruction set](https://wiki.specnext.dev/Extended_Z80_instruction_set) | — | Z80N opcode table (`SWAPNIB`, `MIRROR`, `TEST`, `BSLA`, `MUL D,E`, `NEXTREG`, `PIXELDN`, `LDIRX`, …) |
| [Next hardware reference](https://wiki.specnext.dev/) | — | NextREG map, Layer 2, sprites, tilemap, Copper, DMA, 3× AY |
| [specnext/next-firmware](https://gitlab.com/the-next-team/the-next-firmware) | — | Behaviour of NextZXOS, SD image layout (not bundled) |
| [NEX file format](https://wiki.specnext.dev/NEX_file_format) | — | Header layout, loading-screen bits, bank order, the palette block |
| [tbblue nexload.asm](https://gitlab.com/thesmog358/tbblue/-/blob/master/src/asm/nexload/nexload.asm) | — | Exact loader behaviour: register state left behind, bank order, entry bank, expansion-bus disable |
| [MiSTer ZXNext](https://github.com/MiSTer-devel/ZXNext_MISTer) | GPL-3.0 | FPGA core as an accuracy oracle for video pipeline ordering |
| [DivMMC](https://wiki.specnext.dev/DIVMMC) | — | Port map (`0xE3` control, `0xDF` SPI, `0xE7`/`0xEB` map/unmap), the automap hook addresses, `conmem`/`mapram` |
| [SD Specifications, Part 1: Physical Layer](https://www.sdcard.org/downloads/pls/) | — | SPI command set, R1/R7 responses, the `CSD`/`CID` layouts, `ACMD41` and the HCS bit |

## Conformance suites (to be wired into CI)

* `z80test` — flags, undocumented instructions, MEMPTR.
* `zexall` / `zexdoc` — full instruction coverage.
* `z80ccf` — CCF/SCF flag quirks.
* VHDL-derived per-instruction tests from the Next core.
* Golden-frame PNG hashes for a small set of demos.

## Legal notes

* ROM images are **not** distributed; see `roms/README.md`.
* Snapshot and tape format descriptions are reimplemented from public
  documentation (`worldofspectrum.org` format pages).
* If a future contribution needs to lift code from a GPL project, the whole
  project must move to a GPL-compatible license first.
