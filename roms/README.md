# ROM images

This directory is intentionally **empty**. No ROM images are distributed with
myNext, because the original ZX Spectrum / ZX Spectrum Next ROMs are still
under copyright by Amstrad plc / SpecNext Ltd.

## What to put here

For the 48K machine, drop the 16 KiB ROM as:

```
roms/48.rom        (16 384 bytes)
```

For 128K machines, two ROMs are needed:

```
roms/128-0.rom     (bank 0 — 128K menu / editor)
roms/128-1.rom     (bank 1 — 48 BASIC)
```

`+2` uses the same two-bank layout (`plus2-0.rom`, `plus2-1.rom`), while `+3`
and `+2A` need four banks (`plus3-0.rom` … `plus3-3.rom`).

### How banked ROM sets are loaded

The frontend tries the banked layout first: for a candidate base name such as
`128` it looks for `128-0.rom`, `128-1.rom`, … and concatenates every file that
is exactly 16 384 bytes. If no banked files exist it falls back to a single
`<base>.rom`. That means either layout works:

```sh
./build/mynext roms/128-0.rom        # the rest of the set is picked up
./build/mynext roms/plus3-0.rom      # four-bank +3 set
./build/mynext roms/48.rom           # plain single-bank 48K
```

With no argument the frontend searches the candidate list for the selected
machine type (`48`, `128`, `plus2`, `plus3`) in `roms/`, the working directory
and next to the executable. The machine type can be switched at runtime from the
*Machine* menu, which reloads the matching ROM set.

Snapshots can be dropped onto the window (`.sna`, `.z80`) or opened from the
*File* menu; a snapshot carries its own memory image and does not need a ROM
for the RAM contents, but the ROM is still required for `RST` handlers and
`IM 1` interrupts.

## Where to obtain them

You must dump them from hardware you own, or use a distribution that is legal in
your jurisdiction. Amstrad has publicly permitted distribution of the Spectrum
ROMs for emulation purposes; see the Amstrad / SpecNext terms before
redistributing anything you place here.

## Why they are not bundled

* Keeps the repository free of third-party copyrighted binaries.
* Keeps the project's own license simple.
* Matches what JNEXT, Fuse, ZEsarUX and most other emulators do.

Unit tests never require a real ROM: `tests/test_boot.cpp` assembles a tiny
program directly into ROM bank 0, so `ctest` works on a clean checkout. The
golden boot tests (`tests/test_128k_boot.cpp`) check the real 128K and +3 menus
but skip themselves when the files are absent.
