#include "formats/dsk.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace mynext::formats {
namespace {

constexpr char StandardMagic[] = "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
constexpr char ExtendedMagic[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
constexpr size_t MagicSize = 34;
constexpr size_t TrackInfoSize = 0x100;
constexpr size_t SectorInfoSize = 8;

u16 readLe16(const std::vector<u8>& bytes, size_t offset) {
  return u16(bytes[offset] | (u16(bytes[offset + 1]) << 8));
}

std::string readString(const std::vector<u8>& bytes, size_t offset, size_t length) {
  std::string text;
  for (size_t i = 0; i < length; ++i) {
    const char ch = char(bytes[offset + i]);
    if (ch == '\0') break;
    text.push_back(ch);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

bool readFile(const std::string& path, std::vector<u8>& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (out.empty()) {
    error = path + " is empty";
    return false;
  }
  return true;
}

// One track block. The sector data follows the info table, and DSK images pad
// it to a 256-byte boundary, so every sector is located by walking the table.
bool parseTrack(const std::vector<u8>& bytes, size_t base, size_t limit, DskTrack& track,
                std::string& error) {
  if (base + TrackInfoSize > limit) {
    error = "truncated track block";
    return false;
  }
  if (std::memcmp(bytes.data() + base, "Track-Info\r\n", 12) != 0) {
    error = "missing Track-Info header";
    return false;
  }

  track.track = bytes[base + 0x10];
  track.side = bytes[base + 0x11];
  track.dataRate = bytes[base + 0x12];
  const u8 defaultN = bytes[base + 0x14];
  const u8 sectorCount = bytes[base + 0x15];

  size_t cursor = base + TrackInfoSize;
  for (u8 index = 0; index < sectorCount; ++index) {
    const size_t info = base + 0x18 + size_t(index) * SectorInfoSize;
    if (info + SectorInfoSize > limit) {
      error = "truncated sector table";
      return false;
    }

    DskSector sector;
    sector.c = bytes[info + 0];
    sector.h = bytes[info + 1];
    sector.r = bytes[info + 2];
    sector.n = bytes[info + 3];
    sector.status1 = bytes[info + 4];
    sector.status2 = bytes[info + 5];
    // Some tools store the real length; when they leave it at zero the size
    // comes from the size code, which is what CPCEMU itself assumes.
    const size_t stored = readLe16(bytes, info + 6);
    const size_t length = stored != 0 ? stored : (size_t(128) << (sector.n & 0x07));
    if (sector.n > 7) sector.n = defaultN & 0x07;
    sector.deleted = (sector.status2 & 0x40) != 0;
    if (cursor + length > limit) {
      error = "sector data runs past the end of the track";
      return false;
    }
    sector.data.assign(bytes.begin() + std::ptrdiff_t(cursor),
                       bytes.begin() + std::ptrdiff_t(cursor + length));
    sector.data.resize(sector.size(), 0xE5);
    cursor += length;
    track.sectors.push_back(std::move(sector));
  }

  track.formatted = !track.sectors.empty();
  return true;
}

} // namespace

DskSector* DskTrack::findSector(u8, u8, u8 r, u8 n) {
  // The id is what a loader actually cares about. Some images disagree with the
  // request on the size code, so an exact match wins but a size mismatch still
  // returns the sector instead of failing the read.
  DskSector* fallback = nullptr;
  for (DskSector& sector : sectors) {
    if (sector.r != r) continue;
    if (sector.n == n) return &sector;
    if (fallback == nullptr) fallback = &sector;
  }
  return fallback;
}

const DskSector* DskTrack::findSector(u8 c, u8 h, u8 r, u8 n) const {
  return const_cast<DskTrack*>(this)->findSector(c, h, r, n);
}

DskTrack* DiskImage::findTrack(int track, int side) {
  if (track < 0 || side < 0 || sides <= 0) return nullptr;
  const size_t index = size_t(track) * size_t(sides) + size_t(side);
  if (index >= trackList.size()) return nullptr;
  return &trackList[index];
}

const DskTrack* DiskImage::findTrack(int track, int side) const {
  return const_cast<DiskImage*>(this)->findTrack(track, side);
}

DskTrack& DiskImage::ensureTrack(int track, int side) {
  if (sides <= 0) sides = 1;
  const size_t index = size_t(track) * size_t(sides) + size_t(side);
  if (index >= trackList.size()) trackList.resize(index + 1);
  DskTrack& slot = trackList[index];
  slot.track = track;
  slot.side = side;
  return slot;
}

bool parseDsk(const std::vector<u8>& bytes, DiskImage& out, std::string& error) {
  out = DiskImage{};
  if (bytes.size() < 0x100) {
    error = "file is too small to be a DSK image";
    return false;
  }

  const bool extended = std::memcmp(bytes.data(), ExtendedMagic, MagicSize) == 0;
  if (!extended && std::memcmp(bytes.data(), StandardMagic, MagicSize) != 0) {
    error = "not a CPCEMU or extended DSK image";
    return false;
  }

  out.extended = extended;
  out.creator = readString(bytes, 0x22, 14);
  out.tracks = bytes[0x30];
  out.sides = bytes[0x31];
  if (out.tracks == 0 || out.sides == 0) {
    error = "disk header declares no tracks or sides";
    return false;
  }
  out.trackList.assign(size_t(out.tracks) * size_t(out.sides), DskTrack{});

  const size_t trackCount = size_t(out.tracks) * size_t(out.sides);
  // Extended images keep a per-track size table where the standard format has
  // one fixed size for every track.
  const u16 fixedSize = readLe16(bytes, 0x34);
  if (!extended && fixedSize < TrackInfoSize) {
    error = "invalid track size in the disk header";
    return false;
  }

  size_t offset = TrackInfoSize;
  for (size_t index = 0; index < trackCount; ++index) {
    size_t size = fixedSize;
    if (extended) {
      const size_t tableEntry = 0x34 + index;
      if (tableEntry >= bytes.size()) {
        error = "truncated track size table";
        return false;
      }
      size = size_t(bytes[tableEntry]) * 256;
    }
    DskTrack& track = out.trackList[index];
    track.track = int(index / size_t(out.sides));
    track.side = int(index % size_t(out.sides));
    if (size == 0) continue; // unformatted track
    if (offset + size > bytes.size()) {
      error = "track data runs past the end of the file";
      return false;
    }
    if (!parseTrack(bytes, offset, offset + size, track, error)) return false;
    offset += size;
  }

  return true;
}

bool loadDskFile(const std::string& path, DiskImage& out, std::string& error) {
  std::vector<u8> bytes;
  if (!readFile(path, bytes, error)) return false;
  if (!parseDsk(bytes, out, error)) return false;
  out.path = path;
  return true;
}

} // namespace mynext::formats
