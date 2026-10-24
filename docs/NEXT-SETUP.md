# Running the ZX Spectrum Next

RainbowNext contains no Spectrum or Next ROMs: they are copyrighted, so you
fetch them yourself from the official distribution. This page walks through
that once, end to end, and says exactly which file goes where.

Nothing here is redistributable. The board ROM is © SpecNext Ltd and the
Spectrum ROMs inside it are © Amstrad / Sky plc; NextZXOS is © Garry Lancaster,
esxDOS © Miguel Guerreiro, divMMC © Mario Prato. Keep the files to yourself and
do not commit them.

## What the Next needs

Three separate things, and the machine will not reach its desktop without all
of them:

| Piece | What it is | Size |
|---|---|---|
| Board ROM | the Next's own firmware, four 16K banks in one file | 64 KiB |
| divMMC ROM | the esxDOS card driver, which the boot ROM maps over `0x0000` and refuses to continue without | 8 KiB |
| SD card | NextZXOS itself — the operating system is read off the card, not out of a ROM | an image or a folder |

The Spectrum ROMs (`48.rom`, `128.rom`, `plus3.rom`) are a separate matter; the
Next carries its own copies of those inside the board ROM and does not need
them.

## Where to get them

Everything comes from one page, which always points at the newest release:

**<https://www.specnext.com/latestdistro/>**

The distribution is named by year and month — `24.11` was released in November
2024. Two of the downloads on that page are useful to an emulator; pick one.

### Option A — the emulator build (recommended)

> **System/Next distribution for emulators (1 GB image)**
> `sn-emulator-24.11.zip` — about 50 MB

This is the smallest download that contains everything, and it is laid out for
exactly this job. Unzip it and you get three files and nothing else:

```
sn-emulator-24.11/
├── cspect-next-1gb.img     1 GB   the SD card, ready to mount
├── enNextZX.rom           64 KiB  the board ROM
└── enNxtmmc.rom            8 KiB  the divMMC ROM
```

Note the names: there is no file called `next.rom` in the distribution. The
board ROM is called `enNextZX.rom` (the `en` is the English firmware variant);
`next.rom` is simply the name RainbowNext looks for, so you rename it on the way
in. Same for `enNxtmmc.rom` → `nxtmmc.rom`.

Copy them into the emulator's `roms/` folder like this:

```sh
cp cspect-next-1gb.img  roms/nextsd.img
cp enNextZX.rom         roms/next.rom
cp enNxtmmc.rom         roms/nxtmmc.rom
```

Check you have the right files — the two ROMs are fixed and small enough to
verify by eye:

| File | Bytes | MD5 |
|---|---|---|
| `roms/next.rom` | 65 536 | `8c34d957fb2cbbd0be423cea3b370fd5` |
| `roms/nxtmmc.rom` | 8 192 | `81b6ed2a3dec124eb58ed023c66ea154` |

Then start it:

```sh
./build/rainbow --machine next --sd roms/nextsd.img
```

### Option B — the full distribution folder

> **Complete System/Next distribution**
> `sn-complete-24.11.zip` — about 55 MB, unpacks to roughly 130 MB

This is the tree a real Next's SD card is built from, and it is what you want if
you also intend to run the machine from real hardware, or to keep a directory of
Next software around. It unpacks to a folder like this:

```
sn-complete-24.11/
├── machines/next/           the ROMs — enNextZX.rom, enNxtmmc.rom,
│                            plus 48.rom, 128.rom, plus3.rom and the rest
├── nextzxos/                the operating system
├── dot/                     .dot commands (the Next's utilities)
├── apps/  games/  demos/    software
├── docs/                    the Next manuals, as PDFs
├── sys/  extras/  src/      support files and sources
├── TBBLUE.FW  TBBLUE.TBU    firmware for real hardware — not used by an emulator
└── QXL.WIN                  a 30 MB virtual hard disk for the QL core
```

The ROMs live one level down in `machines/next/`, so:

```sh
cp machines/next/enNextZX.rom  roms/next.rom
cp machines/next/enNxtmmc.rom  roms/nxtmmc.rom
```

The card is the whole folder, which the emulator cannot mount directly — it
needs a raw image. `tools/make_sd_image.py` builds one, writing the partition
table and the FAT32 volume by hand so it needs no root and behaves the same on
macOS and Linux:

```sh
python3 tools/make_sd_image.py sn-complete-24.11 roms/nextsd.img
./build/rainbow --machine next --sd roms/nextsd.img
```

The default image is 512 MB. `--size-mb N` changes it, and the tool tells you
the smallest size that will hold the tree (258 MB for 24.11) if you ask for less
than that. Do not copy `QXL.WIN` in unless you want the QL core; it is a third
of the tree.

## Telling the emulator where things are

Once the files are in `roms/`, nothing else is needed — the frontend looks there
first. There are three other ways, all equivalent:

**The ROM window.** **Machine > ROM images...** lists every machine, with the
Next carrying two extra rows underneath: **divMMC ROM** and **SD card**. Press
`Choose...` on each and the paths are written to `rainbow.cfg`, so the next
start comes up with them already in. `Forget` clears a choice and goes back to
searching `roms/`.

**Command line.** `--rom` takes the board ROM, `--sd` the card, and
`--machine next` selects the machine:

```sh
./build/rainbow --rom roms/next.rom --sd roms/nextsd.img --machine next
```

**Drag and drop.** An `.img` or `.mmc` dropped onto the window is mounted as the
card; a `.rom` replaces the board ROM. Dropping a card is the quickest way to
swap NextZXOS versions without restarting.

The ROM window also reports the divMMC ROM it found. If it says `nxtmmc.rom not
found - the Next will not boot`, that file is the missing one — the board ROM on
its own gets as far as asking the card driver for its `"DV"` signature and then
stops.

## What you should see

The boot goes: board ROM → divMMC ROM answers → esxDOS reads NextZXOS off the
card → the OS takes over, installs its own interrupt handler and starts the
welcome program. With a card built from the 24.11 folder (`Option B` above)
that program runs: a grey page with a **Welcome to NextZXOS** title line and a
body of text, drawn through the OS's own window stream.

The program is navigable, so the keyboard scan works too:

| Key | Effect |
|---|---|
| `ENTER` | next page |
| `P` | previous page |
| `SPACE` | leave the program |
| `D` | disable it — deletes `nextzxos/autoexec.1st` from the card and saves `nextzxos/welcome.bas` |

The program has thirteen pages and `ENTER`/`P` walk through them in both
directions. That is worth trusting as a picture of the video path, because it
has been checked against the machine's own memory: with the card mounted and
the machine settled, the rendered viewport and the 16 KiB bank the ULA reads
(bank 5, i.e. 8 KiB page `0x0A`) agree bit for bit. If the pages look right,
the ULA, the palette, the paging and the key scan are all right.

What does not work yet is **leaving** the program. `SPACE` runs the program's
`RETURN` and the machine drops to the NextBASIC editor rather than to the
NextZXOS desktop; the next keypress moves it onto the OS memory map and clears
the screen, but the desktop is never drawn and the machine idles there. That is
the boot divergence tracked in [`ROADMAP.md`](ROADMAP.md), so treat the Next as
a work in progress rather than a finished machine.

There is a way round it: **delete `nextzxos/autoexec.1st` from the card** and
boot again. With no autoexec program to run, the OS goes straight to the
NextZXOS desktop and its menu draws normally. That is the supported route here
— the emulator ships no cards, so this is one file out of a card you built
yourself.

If you built the card from the CSpect `.img` instead, you may not get as far as
the welcome program; the two routes have not been compared in this build.

Everything else about the Next is implemented and tested: MMU, Layer 2, sprites,
tilemap, Copper, DMA, the TurboSound Next, and `.nex` programs, which load and
run without needing NextZXOS at all. If you just want to see the Next hardware
working, load a `.nex` file:

```sh
./build/rainbow --machine next --rom roms/next.rom --snapshot game.nex
```

## Troubleshooting

| What you see | What it means |
|---|---|
| The machine comes up as a 128K Spectrum | No `roms/next.rom`; the Next deliberately falls back to the 128K set so the emulator still starts |
| `ROM required` overlay at startup | No ROM at all — copy `48.rom` into `roms/`, or pick one in **Machine > ROM images...** |
| `nxtmmc.rom not found` in the ROM window | The 8 KiB divMMC ROM is missing; the Next will not boot without it |
| `NO SD CARD` in the toolbar | No card mounted; NextZXOS has nowhere to come from |
| The card is rejected at load | The file is not a raw sector image, or its size is not a whole number of 512-byte sectors |
| A blank grey screen after leaving the welcome program | Expected in this build — the OS does not redraw its desktop once the welcome program returns; delete `nextzxos/autoexec.1st` from the card to skip it |

## A note on memory

The SD card is held in memory in full, so a 1 GB image costs 1 GB of RAM. An
image built from the folder with `--size-mb 258` is a quarter of that, which
makes it the better choice on a laptop.

Note that the two routes have not been compared in this build. What has been
verified is the folder-built card: it reaches the NextZXOS welcome program,
draws and pages it correctly, and — with `nextzxos/autoexec.1st` removed —
brings up the NextZXOS desktop. If you are comparing behaviour, compare like
with like.

## Licence

The emulator is 0BSD and ships no third-party binaries. The files described on
this page remain under their owners' licences — SpecNext Ltd, Amstrad / Sky plc,
Garry Lancaster, Miguel Guerreiro and Mario Prato — and are for your own use.
Do not redistribute them with RainbowNext or commit them to a repository.
