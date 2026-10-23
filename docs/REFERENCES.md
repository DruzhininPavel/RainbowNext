# References

RainbowNext is written from scratch. The projects below were read to learn *what*
the hardware does and *how* other emulators structure the work. **No GPL code
was copied**; where a file is GPL, only the observable behaviour (timing tables,
port semantics, memory layouts) was reimplemented.

## Timing and ULA behaviour (read as specification)

| Source | License | What was taken from it |
|---|---|---|
| [Fuse](https://github.com/FuseEmulator/fuse-emulator-svn) — `libspectrum/timings.c`, `fuse/spectrum.c`, `fuse/ula.c`, `fuse/display.h` | GPL-2.0 | `base_timings` table for 48K/128K/+2A/+3/Pentagon; contention pattern `{5,4,3,2,1,0,0,6}`; floating-bus column rules; port 0xFE read/write semantics and `ula_default_value` |
| [ZEsarUX](https://github.com/chernandezba/zesarux) | GPL-3.0 | Confirmation of contention ranges and border timing edge cases |
| Fuse `fuse/peripherals/scld.c`, `scld.h` | GPL-2.0 | Timex SCLD: the `scrnmode`/`hirescol` bit split, `ALTDFILE_OFFSET`, the `hires_convert_dec` colour pairs (ink is the complement of paper, both bright) and the `x % 16 > 7` rule that picks the second display file |
| Fuse `libspectrum/rzx.c` | GPL-2.0 | The RZX container layout: the `RZX!` header, the block identifiers and their length-includes-the-header rule, the creator/snapshot/input block fields, and the `0xFFFF` frame-count repeat marker. Reimplemented as `src/core/rzx.*`; no code copied |
| [raxoft/z80test](https://github.com/raxoft/z80test) | MIT | Vendored v1.2a TAP images in `tests/data/z80test/`; `tests/test_z80test.cpp` injects the CODE block into RAM and checks the result line. Found the `DAA`, `INI/IND/OUTI/OUTD` and idle port `0xFE` bugs |
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
| [xpeccy-plus](https://github.com/dotkoval/xpeccy-plus) — `src/libxpeccy/hardware/zxnext.c` | GPL-3.0 | Next register semantics read as specification: the `0x41`/`0x44` palette read-back, the composed `0x69` read, the `0x8E`/`0x8F` legacy remap (`nx_remap`), `nx_rom()` bank selection and the port `0xFF` gate on `0x82` bit 0. Also used as a **black-box oracle**: its headless bench runs the same firmware and card, and the results are compared against ours (see below). No code copied |
| [Timex / Video Modes](https://wiki.specnext.dev/Video_Modes) | — | SCLD mode numbers 0-7, the `0xBF3F`/`0xFF` port, the hi-res colour bits 5:3 |
| [Sprite and Layers System Register](https://wiki.specnext.dev/Sprite_and_Layers_System_Register) | — | `0x15` bits 4:2 priority order and the blending modes 110/111 |
| [ULANext Attribute Byte Format Register](https://wiki.specnext.dev/ULANext_Attribute_Byte_Format_Register) | — | The `0x42` ink mask and the `0x4A` fallback colour for full-ink mode |
| [ULAplus](https://sinclair.wiki.zxnet.co.uk/wiki/ULAplus) | — | The `0xBF3B`/`0xFF3B` register pair, the `GGGRRRBB` byte, the `ink = (FLASH*2+BRIGHT)*16 + INK` and `paper = … + 8` layout, border from entry 8 |
| [Next I/O port map](https://wiki.specnext.dev/Next_ZX_Spectrum_Hardware) | — | Ports `0xE7` (SPI chip select, active low per device) and `0xEB` (SPI data); the same document set is mirrored in `research/sn-complete-24.11/docs/extra-hw/io-port-system/ports.txt` |
| [DivMMC](https://wiki.specnext.dev/DIVMMC) | — | Port map (`0xE3` control, `0xDF` SPI), the automap hook addresses, `conmem`/`mapram`. Kept as the reference for the plugin; no divMMC ships in the core |
| [SD Specifications, Part 1: Physical Layer](https://www.sdcard.org/downloads/pls/) | — | SPI command set, R1/R7 responses, the `CSD`/`CID` layouts, `ACMD41` and the HCS bit |

## Conformance suites

* `z80test` — flags, undocumented instructions, MEMPTR. Wired into
  `rainbow_tests`; the TAP images live in `tests/data/z80test/` under their own
  MIT licence. The suite is started by injecting its CODE block straight into
  RAM, so no copyrighted ROM is needed: `tests/data/test48.rom` is a synthetic
  host firmware generated by `tools/make_test_rom.py` that only implements
  `RST 0x10`, `0x1601` and IM 1.
  * `z80doc` and `z80docflags` pass 160 of 160.
  * `z80full` and `z80flags` report the same six gaps: the two `SCF`+`CCF`
    sequences and the four `->NOP'` block cases that depend on an undefined
    `ED 00`.
  * `z80memptr` repeats each case with a `BIT n,(HL)` after it, the only way to
    observe WZ, and is down to seventeen gaps. Four are the shared `ED 00`
    cases, two are `SCF`+`CCF`, and the rest are WZ values that are still not
    modelled: `ADD`/`ADC`/`SBC HL,rr`, `ADD IX/IY,rr`, `LDIR`/`LDDR`,
    `CPIR`/`CPDR`, `OUTI`/`OUTD`, conditional `JR` and `DJNZ`.
* `zexall` / `zexdoc` — full instruction coverage. Not wired in: both are GPL,
  and redistributing them inside a 0BSD repository is exactly what this project
  avoids. `z80test` already covers the same instruction set.
* `z80ccf` — CCF/SCF flag quirks. Kept in `tests/data/z80test/` but not wired
  in. It does run, and reports 101 of 160, but its own readme says it "assumes
  the genuine Zilog behavior and it will fail half of the tests on CPUs which
  use other variant, so don't bother" — the three `SCF`/`CCF` chip variants it
  distinguishes are a hardware question, not an emulation bug.
* VHDL-derived per-instruction tests from the Next core.
* Golden-frame hashes: `tests/test_golden.cpp` hashes the finished frame of
  eight hand-built scenes. They are not PNG comparisons — the scenes are
  synthetic, so the test needs neither a demo file nor a ROM, and the hash is
  computed over the framebuffer rather than over an encoded image.

## Verifying against an independent emulator

For the Next, "does our core behave like the hardware?" is hard to answer from
the firmware sources alone, because the boot ROM is a closed 64 KB blob. The
cheapest honest oracle is another emulator that runs the same blob.

xpeccy-plus ships a headless bench for exactly this, and it needs no GUI and no
Qt:

```sh
git clone https://github.com/dotkoval/xpeccy-plus /tmp/xp
cd /tmp/xp
cmake -S tests -B build/out/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/out/tests -j8
```

`zxnext-bench` powers the machine on with a firmware handover and writes what
it sees:

```sh
NXSD=/path/to/system-next.img \
  ./build/out/tests/zxnext-bench \
    fw:/path/ennextzx.rom,/path/ennxtmmc.rom \
    600 0x000000 2097152 /tmp/ref
```

`600` is the frame limit; the next two numbers are a memory dump address and
length; the last argument is the output directory. It writes:

| File | Contents |
|---|---|
| `frame_N.txt` | every finished picture, one line per row, one hex RGB333 value per half-pixel |
| `nextreg.txt` | every NextREG write as `frame hc vc cycles reg data` |
| `probe.txt` | every I/O write with the same timing columns |
| `dump.bin` | the requested slice of the 2 MB SRAM |

Comparing it with RainbowNext is then mechanical:

* the last line of the bench's own output prints the CPU's `PC` at the frame
  limit -- ours must match;
* `frame_N.txt` can be turned into an image and compared by eye or by colour
  histogram against `Machine::framebuffer()`;
* `dump.bin` contains the ULA bank, so the 6912 bytes of screen memory can be
  diffed byte for byte against ours.

The result recorded in `docs/ROADMAP.md` gap 17 is that a stock System/Next card
gives a byte-identical ULA bank in both emulators, which is what closed the
"the boot screen is garbage" question: the picture is the firmware's, not ours.

Two caveats: the bench's `fw:` mode is a test harness, not a full boot, so it
does not prove that NextZXOS *starts*; and xpeccy-plus is GPL-3.0, so it is
only ever run as a separate program -- nothing from it is linked or copied.



* ROM images are **not** distributed; see `roms/README.md`.
* Snapshot and tape format descriptions are reimplemented from public
  documentation (`worldofspectrum.org` format pages). RZX has no specification
  document, so its layout was taken from libspectrum's reader and writer.
* An RZX file written by RainbowNext embeds our own `RBNS` state, so it is not
  portable to other emulators; files written by Fuse can be read only if their
  snapshot is one we understand, which in practice means they cannot.
* If a future contribution needs to lift code from a GPL project, the whole
  project must move to a GPL-compatible license first.
