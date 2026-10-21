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
roms/128-0.rom     (bank 0 — 48 BASIC)
roms/128-1.rom     (bank 1 — 128 BASIC editor)
```

The frontend searches `roms/48.rom`, `roms/48.ROM` and `48.rom` relative to the
working directory, and also accepts a ROM path as the first command line
argument:

```sh
./build/mynext roms/48.rom
```

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

Tests never load a real ROM: `tests/test_boot.cpp` assembles a tiny program
directly into ROM bank 0, so `ctest` works on a clean checkout.
