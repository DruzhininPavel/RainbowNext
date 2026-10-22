#!/usr/bin/env python3
"""Builds a raw SD card image out of a directory tree.

The Next boots NextZXOS off an SD card, so exercising that path in the
emulator needs a card image rather than a directory. This lays down an MBR with
a single FAT32 partition and copies the tree into it, which is what a card
formatted on a PC looks like.

Everything is written by hand rather than through mkfs and a mount, because
mounting a filesystem needs privileges we do not want a build tool to ask for.

    tools/make_sd_image.py <source-dir> <image> [--size-mb N]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
PARTITION_START = 2048
RESERVED_SECTORS = 32
SECTORS_PER_CLUSTER = 8
NUM_FATS = 2
# A fixed timestamp keeps two runs over the same tree byte-identical.
DOS_DATE = ((2024 - 1980) << 9) | (1 << 5) | 1
DOS_TIME = 0

# Characters FAT allows in a short name, and the ones Windows refuses in a long
# one. Anything outside them makes the name need the tilde form.
SHORT_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
LONG_BAD = set('"*/:<>?\\|')


class Node:
    def __init__(self, name: str, path: Path, is_dir: bool, size: int = 0):
        self.name = name
        self.path = path
        self.is_dir = is_dir
        self.size = size
        self.short = ""
        self.first_cluster = 0
        self.children: list[Node] = []


def build_tree(root: Path) -> Node:
    node = Node(root.name, root, True)

    def walk(dir_node: Node) -> None:
        for entry in sorted(dir_node.path.iterdir(), key=lambda p: p.name):
            # The bookkeeping files a Mac leaves behind mean nothing to
            # NextZXOS and only take up directory slots.
            if entry.name == ".DS_Store" or entry.name.startswith("._"):
                continue
            if entry.is_symlink():
                continue
            if entry.is_dir():
                child = Node(entry.name, entry, True)
                walk(child)
            elif entry.is_file():
                child = Node(entry.name, entry, False, entry.stat().st_size)
            else:
                continue
            dir_node.children.append(child)

    walk(node)
    return node


def lfn_checksum(short: str) -> int:
    total = 0
    for ch in short:
        total = (((total & 1) << 7) + (total >> 1) + ord(ch)) & 0xFF
    return total


def needs_long_name(name: str) -> bool:
    if name in (".", ".."):
        return False
    if not name or name != name.upper() or name.count(".") > 1:
        return True
    base, _, ext = name.partition(".")
    return any(c not in SHORT_OK for c in base + ext) or len(base) > 8 or len(ext) > 3


def make_short_name(name: str, taken: set[str]) -> str:
    if not needs_long_name(name) and name not in taken:
        taken.add(name)
        return name

    base, _, ext = name.upper().rpartition(".")
    if not base:
        base, ext = ext, ""
    base = "".join(c if c in SHORT_OK else "_" for c in base)[:8]
    ext = "".join(c if c in SHORT_OK else "_" for c in ext)[:3]
    if not base:
        base = "_"
    candidate = f"{base}.{ext}" if ext else base
    tail = 1
    while candidate in taken:
        suffix = f"~{tail}"
        candidate = base[: 8 - len(suffix)] + suffix
        candidate += f".{ext}" if ext else ""
        tail += 1
    taken.add(candidate)
    return candidate


def assign_short_names(node: Node) -> None:
    taken: set[str] = set()
    for child in node.children:
        child.short = make_short_name(child.name, taken)
    for child in node.children:
        if child.is_dir:
            assign_short_names(child)


def dir_entry_count(node: Node) -> int:
    """Directory slots the node needs, long-name slots included."""
    count = 0
    if node.name not in ("", "/"):
        count += 2  # "." and ".."
    for child in node.children:
        count += 1
        if child.short != child.name:
            count += (len(child.name) + 12) // 13
    return max(count + 1, 1)  # the trailing 0x00 that ends the listing


def lfn_entries(name: str, short: str) -> list[bytes]:
    checksum = lfn_checksum(short)
    encoded = name.encode("utf-16-le")
    chunks = [encoded[i : i + 26] for i in range(0, len(encoded), 26)]
    out = []
    for index, chunk in enumerate(reversed(chunks)):
        ordinal = len(chunks) - index
        chunk = chunk.ljust(26, b"\x00")
        if len(chunk) < 26:
            chunk += b"\xff\xff"
        if index == 0:
            ordinal |= 0x40
        entry = bytearray(32)
        entry[0] = ordinal
        entry[1:11] = chunk[0:10]
        entry[11] = 0x0F
        entry[12] = 0x00
        entry[13] = checksum
        entry[14:26] = chunk[10:22]
        entry[26:28] = b"\x00\x00"
        entry[28:32] = chunk[22:26]
        out.append(bytes(entry))
    return out


def short_entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    if name in (".", ".."):
        base, ext = name.ljust(11), b""
        raw = name.ljust(11)[:11].encode("ascii")
    else:
        base, _, ext = name.partition(".")
        raw = (base.ljust(8)[:8] + ext.ljust(3)[:3]).encode("ascii")
    entry = bytearray(32)
    entry[0:11] = raw
    entry[11] = attr
    entry[12] = 0x18  # no LFN, no read-only oddities
    struct.pack_into("<H", entry, 14, DOS_TIME)
    struct.pack_into("<H", entry, 16, DOS_DATE)
    struct.pack_into("<H", entry, 18, DOS_DATE)
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 22, DOS_TIME)
    struct.pack_into("<H", entry, 24, DOS_DATE)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def directory_entries(node: Node) -> bytes:
    out = bytearray()
    if node.name not in ("", "/"):
        parent = node.parent_cluster if hasattr(node, "parent_cluster") else 0
        out += short_entry(".", 0x10, node.first_cluster, 0)
        out += short_entry("..", 0x10, parent, 0)
    for child in node.children:
        if child.short != child.name:
            out += b"".join(lfn_entries(child.name, child.short))
        attr = 0x10 if child.is_dir else 0x20
        size = 0 if child.is_dir else child.size
        out += short_entry(child.short, attr, child.first_cluster, size)
    return bytes(out)


class Fat32:
    def __init__(self, total_sectors: int):
        self.total = total_sectors
        fat_sectors = 1
        while True:
            data = total_sectors - RESERVED_SECTORS - NUM_FATS * fat_sectors
            clusters = data // SECTORS_PER_CLUSTER
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        self.fat_sectors = fat_sectors
        self.data_start = RESERVED_SECTORS + NUM_FATS * fat_sectors
        self.clusters = (total_sectors - self.data_start) // SECTORS_PER_CLUSTER
        if self.clusters < 65525:
            raise SystemExit("image too small for FAT32; ask for at least 64 MB")
        self.cluster_bytes = SECTORS_PER_CLUSTER * SECTOR
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_cluster = 2
        self.data = bytearray(self.clusters * self.cluster_bytes)

    def allocate(self, count: int) -> int:
        if count <= 0:
            return 0
        first = self.next_cluster
        if first + count > self.clusters + 2:
            raise SystemExit("image is full")
        for index in range(count):
            cluster = first + index
            self.fat[cluster] = cluster + 1 if index + 1 < count else 0x0FFFFFFF
        self.next_cluster += count
        return first

    def write_clusters(self, first: int, payload: bytes) -> None:
        offset = (first - 2) * self.cluster_bytes
        self.data[offset : offset + len(payload)] = payload


def cluster_count(fat: Fat32, size: int) -> int:
    return (size + fat.cluster_bytes - 1) // fat.cluster_bytes


def layout(node: Node, fat: Fat32, parent_cluster: int) -> None:
    node.parent_cluster = parent_cluster
    if node.is_dir:
        slots = dir_entry_count(node)
        node.dir_bytes = slots * 32
        node.first_cluster = fat.allocate(cluster_count(fat, node.dir_bytes))
        for child in node.children:
            layout(child, fat, node.first_cluster)
    else:
        node.first_cluster = fat.allocate(cluster_count(fat, node.size))


def emit(node: Node, fat: Fat32, root_cluster: int) -> None:
    if not node.is_dir:
        payload = node.path.read_bytes()
        if len(payload) != node.size:
            raise SystemExit(f"{node.path}: changed size while imaging")
        fat.write_clusters(node.first_cluster, payload)
        return
    entries = directory_entries(node)
    entries += b"\x00" * (node.dir_bytes - len(entries))
    fat.write_clusters(node.first_cluster, entries)
    for child in node.children:
        emit(child, fat, root_cluster)


def boot_sector(fat: Fat32, volume_id: int) -> bytes:
    sector = bytearray(SECTOR)
    sector[0:3] = b"\xeb\x58\x90"
    sector[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", sector, 11, SECTOR)
    sector[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", sector, 14, RESERVED_SECTORS)
    sector[16] = NUM_FATS
    struct.pack_into("<H", sector, 17, 0)  # root entries: none on FAT32
    struct.pack_into("<H", sector, 19, 0)  # 16-bit sector count: unused
    sector[21] = 0xF8  # fixed disk
    struct.pack_into("<H", sector, 22, 0)  # FAT16 size: unused
    struct.pack_into("<H", sector, 24, 32)  # sectors per track
    struct.pack_into("<H", sector, 26, 64)  # heads
    struct.pack_into("<I", sector, 28, PARTITION_START)
    struct.pack_into("<I", sector, 32, fat.total)
    struct.pack_into("<I", sector, 36, fat.fat_sectors)
    struct.pack_into("<H", sector, 40, 0)  # mirroring and drive number flags
    struct.pack_into("<H", sector, 42, 0)  # version 0.0
    struct.pack_into("<I", sector, 44, 2)  # root directory cluster
    struct.pack_into("<H", sector, 48, 1)  # FSInfo sector
    struct.pack_into("<H", sector, 50, 6)  # backup boot sector
    sector[64] = 0x80
    sector[66] = 0x29
    struct.pack_into("<I", sector, 67, volume_id)
    sector[71:82] = b"NEXTSD     "
    sector[82:90] = b"FAT32   "
    sector[510:512] = b"\x55\xaa"
    return bytes(sector)


def fsinfo_sector(fat: Fat32) -> bytes:
    sector = bytearray(SECTOR)
    sector[0:4] = b"RRaA"
    sector[484:488] = b"rrAa"
    free = fat.clusters - (fat.next_cluster - 2)
    struct.pack_into("<I", sector, 488, free)
    struct.pack_into("<I", sector, 492, fat.next_cluster)
    sector[510:512] = b"\x55\xaa"
    return bytes(sector)


def fat_table(fat: Fat32) -> bytes:
    raw = b"".join(struct.pack("<I", value) for value in fat.fat)
    raw += b"\x00" * (fat.fat_sectors * SECTOR - len(raw))
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="directory to copy onto the card")
    parser.add_argument("image", type=Path, help="raw image to write")
    parser.add_argument("--size-mb", type=int, default=512)
    args = parser.parse_args()

    if not args.source.is_dir():
        raise SystemExit(f"{args.source}: not a directory")
    if args.size_mb < 64:
        raise SystemExit("--size-mb must be at least 64")

    total_sectors = args.size_mb * 2048 - PARTITION_START
    fat = Fat32(total_sectors)

    root = build_tree(args.source)
    root.name = ""
    assign_short_names(root)
    layout(root, fat, 0)
    emit(root, fat, root.first_cluster)

    volume_id = 0x4E455854
    with open(args.image, "wb") as out:
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack(
            "<B3sB3sII", 0x00, b"\x00\x02\x00", 0x0C, b"\xfe\xff\xff",
            PARTITION_START, total_sectors)
        mbr[510:512] = b"\x55\xaa"
        out.write(mbr)
        out.write(b"\x00" * ((PARTITION_START - 1) * SECTOR))

        boot = boot_sector(fat, volume_id)
        out.write(boot)
        out.write(fsinfo_sector(fat))
        out.write(b"\x00" * (4 * SECTOR))
        out.write(boot)
        out.write(b"\x00" * ((RESERVED_SECTORS - 7) * SECTOR))

        table = fat_table(fat)
        out.write(table)
        out.write(table)

        out.write(fat.data)
        # The data region is a whole number of clusters, which can leave a few
        # sectors of the partition unused; pad them so the image ends exactly
        # where the card would.
        written = out.tell()
        total_bytes = (PARTITION_START + total_sectors) * SECTOR
        out.write(b"\x00" * (total_bytes - written))

    used = (fat.next_cluster - 2) * fat.cluster_bytes
    print(f"{args.image}: {args.size_mb} MB, {fat.clusters} clusters, "
          f"{used // 1024} KiB of files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
