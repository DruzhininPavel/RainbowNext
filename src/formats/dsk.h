#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow::formats {

// One sector as stored in a DSK image. `data` always holds 128 << n bytes, so
// the controller can hand out a fixed-size block without knowing the geometry
// the image was written with.
struct DskSector {
  u8 c = 0; // cylinder
  u8 h = 0; // head
  u8 r = 1; // sector id
  u8 n = 2; // size code: 128 << n bytes
  u8 status1 = 0;
  u8 status2 = 0;
  bool deleted = false;
  std::vector<u8> data;

  size_t size() const { return size_t(128) << n; }
};

struct DskTrack {
  int track = 0;
  int side = 0;
  u8 dataRate = 0;
  bool formatted = false;
  std::vector<DskSector> sectors;

  DskSector* findSector(u8 c, u8 h, u8 r, u8 n);
  const DskSector* findSector(u8 c, u8 h, u8 r, u8 n) const;
};

// A disk image decoded from the CPCEMU `.dsk` or the extended `.edsk`
// container. Both are the formats the +3 emulators and tools agree on, so one
// in-memory representation covers everything the µPD765 needs.
struct DiskImage {
  std::string path;
  std::string creator;
  int tracks = 0; // tracks per side
  int sides = 0;
  bool extended = false;

  // Indexed as track * sides + side.
  std::vector<DskTrack> trackList;

  bool empty() const { return trackList.empty(); }
  DskTrack* findTrack(int track, int side);
  const DskTrack* findTrack(int track, int side) const;
  // Creates the track on demand, for FORMAT TRACK on a previously empty track.
  DskTrack& ensureTrack(int track, int side);
};

bool parseDsk(const std::vector<u8>& bytes, DiskImage& out, std::string& error);
bool loadDskFile(const std::string& path, DiskImage& out, std::string& error);

} // namespace rainbow::formats
