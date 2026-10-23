# ROM images

This directory is intentionally **empty**. No ROM images are distributed with
RainbowNext, because the original ZX Spectrum / ZX Spectrum Next ROMs are still
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

For the Next, drop the 64 KiB board ROM (four 16 KiB banks) as:

```
roms/next.rom      (65 536 bytes)
roms/next-0.rom … next-3.rom
```

The Next needs a **second** image: the 8 KiB divMMC / esxDOS ROM that ships
beside the board ROM in the official distribution (`enNxtmmc.rom`):

```
roms/nxtmmc.rom    (8 192 bytes)
```

The board ROM does not contain the divMMC code. While booting it loads `0x1F01`
into `BC` and jumps through `RST 0x38`, expecting the module to be mapped in at
that address; without the image the CPU walks off into empty RAM and the machine
restarts forever. The frontend therefore loads the divMMC ROM automatically for
the Next (and only for the Next), looking for `nxtmmc`, `nextmmc`, `divmmc` and
`esxdos` with a `.rom` or `.bin` extension in `roms/`, next to the executable,
and in the working directory. On the other machines the divMMC stays the user's
choice and is not touched.

## Picking the images once

Instead of passing `--rom` on every start, open **Machine > ROM images...** and
press `Choose...` on the machine you want. The path is written to `rainbow.cfg`
next to the other settings and is used from then on; `Forget` clears it and goes
back to searching `roms/`. Each machine keeps its own images, so switching
between a 128K and a Next never picks up the wrong-sized ROM, and the machine
itself is remembered too, so the next start comes up where the last one ended.

The Next's row has a divMMC sub-row, which is where the companion image above
belongs — the emulator then loads it together with the board ROM instead of
telling you later that it is missing. The toolbar shows `NO DIVMMC` while a
machine that needs one does not have it; the chip disappears once the image is
loaded.

If no `next` set is found the Next falls back to the `128` set, so a Next without
its own ROM still boots the 128K machine. The ROMs only cover the board and
divMMC; NextZXOS lives on the SD card image and is loaded separately with `--sd`
(see below).

### How banked ROM sets are loaded

The frontend tries the banked layout first: for a candidate base name such as
`128` it looks for `128-0.rom`, `128-1.rom`, … and concatenates every file that
is exactly 16 384 bytes. If no banked files exist it falls back to a single
`<base>.rom`. That means either layout works:

```sh
./build/rainbow roms/128-0.rom        # the rest of the set is picked up
./build/rainbow roms/plus3-0.rom      # four-bank +3 set
./build/rainbow roms/48.rom           # plain single-bank 48K
```

With no argument the frontend searches the candidate list for the selected
machine type (`48`, `128`, `plus2`, `plus3`, `pentagon`, `next`) in `roms/`, the
working directory and next to the executable. The machine type can be switched at
runtime from the *Machine* menu, which reloads the matching ROM set.

The Next also takes an SD card image, which is where NextZXOS and its ROMs live:

```sh
./build/rainbow --sd nextzxos.img
```

An `.img` dropped onto the window is mounted as the card as well; the card is
owned by the frontend, so it survives a machine change.

The official distribution ships as a directory tree rather than an image, so
`tools/make_sd_image.py` turns it into a card. It writes the partition table,
the FAT32 volume and every file by hand, which means it needs no privileges and
works the same on macOS and Linux:

```sh
python3 tools/make_sd_image.py /path/to/sn-complete-24.11 roms/nextsd.img
./build/rainbow --sd roms/nextsd.img
```

The default size is 512 MB; `--size-mb` changes it and the image is refused if
the tree does not fit. Names are stored in their original case and the
Mac bookkeeping files are left out.

Snapshots can be dropped onto the window (`.sna`, `.z80`) or opened from the
*File* menu; a snapshot carries its own memory image and does not need a ROM
for the RAM contents, but the ROM is still required for `RST` handlers and
`IM 1` interrupts.

## Where to obtain them

You must dump them from hardware you own, or use a distribution that is legal in
your jurisdiction. Amstrad has publicly permitted distribution of the Spectrum
ROMs for emulation purposes; see the Amstrad / SpecNext terms before
redistributing anything you place here.

The official SpecNext distribution (<https://www.specnext.com/latestdistro/>) is
the usual source for the Next files: it ships the board ROM used by emulators and
the NextZXOS SD card image. Nothing from it is committed here.

## Why they are not bundled

* Keeps the repository free of third-party copyrighted binaries.
* Keeps the project's own license simple.
* Matches what JNEXT, Fuse, ZEsarUX and most other emulators do.

Unit tests never require a real ROM: `tests/test_boot.cpp` assembles a tiny
program directly into ROM bank 0, so `ctest` works on a clean checkout. The
golden boot tests (`tests/test_128k_boot.cpp`) check the real 128K and +3 menus
but skip themselves when the files are absent.
