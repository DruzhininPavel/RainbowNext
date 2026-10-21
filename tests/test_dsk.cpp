#include "test_framework.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/upd765.h"
#include "formats/dsk.h"

using mynext::Upd765;
using mynext::formats::DiskImage;
using mynext::formats::DskSector;
using mynext::formats::DskTrack;
using mynext::u16;
using mynext::u8;

namespace {

constexpr size_t TrackInfoSize = 0x100;

// Builds a CPCEMU image the way real tools write one: a 256-byte header, then
// one Track-Info block per track followed by its sector payloads.
std::vector<u8> makeDsk(bool extended, int tracks, int sides, int sectorCount, int sizeCode) {
  const size_t sectorLength = size_t(128) << sizeCode;
  const size_t rawTrack = TrackInfoSize + sectorLength * size_t(sectorCount);
  const size_t trackSize = (rawTrack + 0xFF) & ~size_t(0xFF);

  std::vector<u8> bytes(TrackInfoSize, 0);
  const char* magic =
      extended ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n" : "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
  std::memcpy(bytes.data(), magic, 34);
  std::memcpy(bytes.data() + 0x22, "myNext", 6);
  bytes[0x30] = u8(tracks);
  bytes[0x31] = u8(sides);

  if (extended) {
    for (int index = 0; index < tracks * sides; ++index) {
      bytes[0x34 + size_t(index)] = u8(trackSize / 256);
    }
  } else {
    bytes[0x34] = u8(trackSize & 0xFF);
    bytes[0x35] = u8(trackSize >> 8);
  }

  for (int index = 0; index < tracks * sides; ++index) {
    const size_t base = bytes.size();
    bytes.resize(base + trackSize, 0);
    std::memcpy(bytes.data() + base, "Track-Info\r\n", 12);
    bytes[base + 0x10] = u8(index / sides);
    bytes[base + 0x11] = u8(index % sides);
    bytes[base + 0x12] = 2; // 250 kbit/s
    bytes[base + 0x14] = u8(sizeCode);
    bytes[base + 0x15] = u8(sectorCount);

    for (int sector = 0; sector < sectorCount; ++sector) {
      const size_t info = base + 0x18 + size_t(sector) * 8;
      bytes[info + 0] = u8(index / sides); // C
      bytes[info + 1] = u8(index % sides); // H
      bytes[info + 2] = u8(sector + 0x41); // R
      bytes[info + 3] = u8(sizeCode);      // N
      bytes[info + 6] = u8(sectorLength & 0xFF);
      bytes[info + 7] = u8(sectorLength >> 8);

      const size_t payload = base + TrackInfoSize + size_t(sector) * sectorLength;
      for (size_t byte = 0; byte < sectorLength; ++byte) {
        bytes[payload + byte] = u8(sector * 0x10 + (byte & 0x0F));
      }
    }
  }
  return bytes;
}

// Drives the controller the way the +3 ROM does: command bytes to one port,
// polling the status register before every data byte.
struct Controller {
  Upd765 fdc;
  DiskImage disk;

  bool insert(std::vector<u8> image) {
    std::string error;
    if (!mynext::formats::parseDsk(image, disk, error)) return false;
    fdc.reset();
    fdc.setDisk(&disk);
    fdc.setMotor(true);
    return true;
  }

  void command(std::initializer_list<u8> bytes) {
    for (u8 byte : bytes) fdc.writeCommand(byte);
  }

  std::vector<u8> readBytes(size_t count) {
    std::vector<u8> out;
    for (size_t i = 0; i < count; ++i) out.push_back(fdc.readData());
    return out;
  }

  std::vector<u8> result() { return readBytes(7); }

  bool canRead() const { return (fdc.readStatus() & Upd765::MsrRqm) != 0; }
};

} // namespace

TEST(dsk_parses_a_standard_image) {
  const std::vector<u8> bytes = makeDsk(false, 2, 1, 9, 2);
  DiskImage disk;
  std::string error;

  CHECK(mynext::formats::parseDsk(bytes, disk, error));
  CHECK(error.empty());
  CHECK_EQ(disk.tracks, 2);
  CHECK_EQ(disk.sides, 1);
  CHECK(!disk.extended);
  CHECK(disk.creator == "myNext");

  const DskTrack* track = disk.findTrack(1, 0);
  CHECK(track != nullptr);
  CHECK(track->formatted);
  CHECK_EQ(track->sectors.size(), 9);
  CHECK_EQ(track->sectors[0].r, 0x41);
  CHECK_EQ(track->sectors[0].n, 2);
  CHECK_EQ(track->sectors[0].data.size(), 512);
  // The generator fills sector n with 0x10*n + (offset & 0x0F).
  CHECK_EQ(track->sectors[3].data[0], 0x30);
  CHECK_EQ(track->sectors[3].data[0x1F], 0x3F);
}

TEST(dsk_parses_an_extended_image_with_a_per_track_table) {
  const std::vector<u8> bytes = makeDsk(true, 3, 2, 4, 1);
  DiskImage disk;
  std::string error;

  CHECK(mynext::formats::parseDsk(bytes, disk, error));
  CHECK(disk.extended);
  CHECK_EQ(disk.tracks, 3);
  CHECK_EQ(disk.sides, 2);
  CHECK_EQ(disk.trackList.size(), 6);

  // Track index = track * sides + side, so (2, 1) is the last one.
  const DskTrack* track = disk.findTrack(2, 1);
  CHECK(track != nullptr);
  CHECK_EQ(track->track, 2);
  CHECK_EQ(track->side, 1);
  CHECK_EQ(track->sectors.size(), 4);
  CHECK_EQ(track->sectors[1].data.size(), 256);
}

TEST(dsk_skips_unformatted_tracks) {
  std::vector<u8> bytes = makeDsk(true, 2, 1, 4, 2);
  // Zero the size of the second track: the format uses that for "never written".
  bytes[0x35] = 0;

  DiskImage disk;
  std::string error;
  CHECK(mynext::formats::parseDsk(bytes, disk, error));
  CHECK(disk.findTrack(0, 0) != nullptr);
  CHECK(disk.findTrack(0, 0)->formatted);
  CHECK_EQ(disk.findTrack(0, 0)->sectors.size(), 4);
  // Track 1 keeps its slot so the geometry stays addressable, but carries no
  // sectors, which is what "never written" means to the controller.
  CHECK(disk.findTrack(1, 0) != nullptr);
  CHECK(!disk.findTrack(1, 0)->formatted);
  CHECK_EQ(disk.findTrack(1, 0)->sectors.size(), 0);
}

TEST(dsk_rejects_files_that_are_not_images) {
  DiskImage disk;
  std::string error;

  CHECK(!mynext::formats::parseDsk(std::vector<u8>(0x200, 0xAA), disk, error));
  CHECK(!error.empty());

  error.clear();
  CHECK(!mynext::formats::parseDsk(std::vector<u8>(16, 0), disk, error));
  CHECK(!error.empty());
}

TEST(dsk_rejects_a_truncated_track) {
  std::vector<u8> bytes = makeDsk(false, 1, 1, 9, 2);
  bytes.resize(bytes.size() - 1024);

  DiskImage disk;
  std::string error;
  CHECK(!mynext::formats::parseDsk(bytes, disk, error));
  CHECK(!error.empty());
}

TEST(dsk_finds_a_sector_by_id_and_prefers_the_exact_size_code) {
  DskTrack track;
  DskSector first;
  first.r = 1;
  first.n = 1;
  DskSector second;
  second.r = 1;
  second.n = 2;
  track.sectors = {first, second};

  CHECK_EQ(track.findSector(0, 0, 1, 2)->n, 2);
  CHECK_EQ(track.findSector(0, 0, 1, 1)->n, 1);
  // An unknown id misses; a known id with an odd size code still resolves.
  CHECK(track.findSector(0, 0, 9, 2) == nullptr);
  CHECK_EQ(track.findSector(0, 0, 1, 6)->n, 1);
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

TEST(fdc_reports_not_ready_without_a_disk) {
  Upd765 fdc;
  fdc.reset();

  CHECK(!fdc.hasDisk());
  fdc.writeCommand(0x0A); // READ ID
  fdc.writeCommand(0x00);
  const std::vector<u8> result = {fdc.readData(), fdc.readData(), fdc.readData(),
                                  fdc.readData(), fdc.readData(), fdc.readData(),
                                  fdc.readData()};
  CHECK_EQ(result[0] & 0xC0, 0x40); // abnormal termination
  CHECK_EQ(result[1] & 0x04, 0x04); // no data
}

TEST(fdc_specify_leaves_the_controller_idle) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  c.command({0x03, 0xAF, 0x12}); // SPECIFY
  CHECK(!c.fdc.busy());
  CHECK_EQ(c.fdc.readStatus() & Upd765::MsrRqm, Upd765::MsrRqm);
}

TEST(fdc_sense_drive_status_reports_ready_and_track_zero) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 2, 1, 9, 2)));

  c.command({0x04, 0x00}); // SENSE DRIVE STATUS, unit 0
  const std::vector<u8> result = {c.fdc.readData(), c.fdc.readData()};
  CHECK_EQ(result[0] & 0x03, 0x00);
  CHECK_EQ(result[1] & 0x20, 0x20); // ready
  CHECK_EQ(result[1] & 0x10, 0x10); // at track 0
}

TEST(fdc_seek_moves_the_head_and_sense_interrupt_confirms_it) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 3, 1, 9, 2)));

  c.command({0x0F, 0x00, 0x02}); // SEEK to cylinder 2
  CHECK(!c.fdc.busy());
  CHECK_EQ(c.fdc.cylinder(), 2);

  c.command({0x08}); // SENSE INTERRUPT STATUS
  std::vector<u8> result;
  for (int i = 0; i < 7; ++i) result.push_back(c.fdc.readData());
  CHECK_EQ(result[0] & 0x20, 0x20); // seek end
  CHECK_EQ(result[3], 2);           // present cylinder number
  CHECK(!c.fdc.busy());

  // A second SENSE INTERRUPT has nothing to report.
  c.command({0x08});
  CHECK_EQ(c.fdc.readData() & 0xC0, 0x80);
}

TEST(fdc_recalibrate_returns_to_track_zero) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 3, 1, 9, 2)));

  c.command({0x0F, 0x00, 0x02});
  CHECK_EQ(c.fdc.cylinder(), 2);
  c.command({0x07, 0x00}); // RECALIBRATE
  CHECK_EQ(c.fdc.cylinder(), 0);
}

TEST(fdc_read_id_returns_the_first_sector_header) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 2, 1, 9, 2)));

  c.command({0x0A, 0x00}); // READ ID
  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x00); // normal termination
  CHECK_EQ(result[3], 0);           // C
  CHECK_EQ(result[4], 0);           // H
  CHECK_EQ(result[5], 0x41);        // R
  CHECK_EQ(result[6], 2);           // N
  CHECK(!c.fdc.busy());
}

TEST(fdc_read_data_streams_the_sector_bytes) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 2, 1, 9, 2)));

  c.command({0x06, 0x00, 0x00, 0x00, 0x41, 0x02, 0x41, 0x2A, 0xFF});
  CHECK(c.fdc.readStatus() & Upd765::MsrNdma);
  CHECK(c.fdc.readStatus() & Upd765::MsrDio);

  std::vector<u8> data;
  for (int i = 0; i < 512; ++i) data.push_back(c.fdc.readData());
  // Sector n is filled with 0x10*n + (offset & 0x0F), so id 0x41 repeats 0x00..0x0F.
  CHECK_EQ(data[0], 0x00);
  CHECK_EQ(data[0x0F], 0x0F);
  CHECK_EQ(data[0x10], 0x00);
  CHECK_EQ(data[0x1FF], 0x0F);

  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x00); // normal
  CHECK_EQ(result[1], 0x00);        // no error
  CHECK_EQ(result[5], 0x41);        // last sector read
}

TEST(fdc_read_data_can_chain_several_sectors) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  // R = 0x41, EOT = 0x43 -> three sectors of 512 bytes.
  c.command({0x06, 0x00, 0x00, 0x00, 0x41, 0x02, 0x43, 0x2A, 0xFF});
  const std::vector<u8> data = c.readBytes(3 * 512);
  CHECK_EQ(data.size(), 3u * 512u);
  CHECK_EQ(data[0], 0x00);
  CHECK_EQ(data[512], 0x10);
  CHECK_EQ(data[1024], 0x20);

  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x00);
  CHECK_EQ(result[5], 0x43);
}

TEST(fdc_read_data_reports_a_missing_sector) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  c.command({0x06, 0x00, 0x00, 0x00, 0x7F, 0x02, 0x7F, 0x2A, 0xFF});
  // The controller still enters the data phase, but with an empty buffer it
  // goes straight to the result phase with "no data".
  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x40); // abnormal
  CHECK_EQ(result[1] & 0x04, 0x04); // no data
}

TEST(fdc_write_data_updates_the_image) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  c.command({0x05, 0x00, 0x00, 0x00, 0x41, 0x02, 0x41, 0x2A, 0xFF});
  for (int i = 0; i < 512; ++i) c.fdc.writeData(u8(i & 0xFF));

  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x00);

  const DskTrack* track = c.disk.findTrack(0, 0);
  CHECK(track != nullptr);
  const DskSector* sector = track->findSector(0, 0, 0x41, 2);
  CHECK(sector != nullptr);
  CHECK_EQ(sector->data[0], 0x00);
  CHECK_EQ(sector->data[0xFF], 0xFF);
  CHECK_EQ(sector->data[0x100], 0x00);
}

TEST(fdc_format_track_builds_the_sector_table) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  // FORMAT TRACK: N = 2 (512 bytes), SC = 2, GPL = 0x2A, filler = 0xE5.
  c.command({0x0D, 0x00, 0x02, 0x02, 0x2A, 0xE5});
  const u8 ids[] = {0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x02, 0x02};
  for (u8 byte : ids) c.fdc.writeData(byte);

  const std::vector<u8> result = c.result();
  CHECK_EQ(result[0] & 0xC0, 0x00);

  const DskTrack* track = c.disk.findTrack(0, 0);
  CHECK(track != nullptr);
  CHECK_EQ(track->sectors.size(), 2);
  CHECK_EQ(track->sectors[0].r, 1);
  CHECK_EQ(track->sectors[1].r, 2);
  CHECK_EQ(track->sectors[0].data.size(), 512);
  CHECK_EQ(track->sectors[0].data[0], 0xE5);
}

TEST(fdc_rejects_an_unknown_command) {
  Controller c;
  CHECK(c.insert(makeDsk(false, 1, 1, 9, 2)));

  c.command({0x1F}); // not a command
  CHECK_EQ(c.fdc.readData() & 0xC0, 0x80); // invalid
}

TEST(fdc_write_without_a_disk_reports_no_data) {
  Upd765 fdc;
  fdc.reset();
  for (u8 byte : {u8(0x05), u8(0x00), u8(0x00), u8(0x00), u8(0x41), u8(0x02), u8(0x41), u8(0x2A),
                  u8(0xFF)}) {
    fdc.writeCommand(byte);
  }
  CHECK_EQ(fdc.readData() & 0xC0, 0x40);
  CHECK_EQ(fdc.readData() & 0x04, 0x04);
}
