#include "test_framework.h"

#include <string>
#include <vector>

#include "core/machine.h"
#include "core/sdcard.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::SdCard;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;

namespace {

// 64 sectors of recognizable data, so a read can be checked by content rather
// than just by length.
std::vector<u8> makeCard(int sectors = 64) {
  std::vector<u8> image(size_t(sectors) * 512);
  for (size_t i = 0; i < image.size(); ++i) image[i] = u8(i & 0xFF);
  return image;
}

// Every sector filled with one value that depends on its index, so a multi-block
// transfer can be checked sector by sector.
std::vector<u8> makeSectorCard(int sectors = 16) {
  std::vector<u8> image(size_t(sectors) * 512);
  for (int sector = 0; sector < sectors; ++sector) {
    for (int i = 0; i < 512; ++i) image[size_t(sector) * 512 + size_t(i)] = u8(0x40 + sector);
  }
  return image;
}

// Sends a command frame and collects everything the card shifts back, stopping
// once it has produced `expected` bytes or gone quiet.
struct Exchange {
  std::vector<u8> bytes;

  bool contains(u8 value) const {
    for (u8 b : bytes) {
      if (b == value) return true;
    }
    return false;
  }
};

Exchange command(SdCard& card, u8 cmd, u32 arg, size_t extra = 0) {
  Exchange out;
  // Clear anything the previous command left in the shift register, so each
  // exchange starts from the idle state. In the command state 0xFF bytes are
  // ignored, so this is safe even when there is nothing to clear.
  for (int i = 0; i < 4; ++i) card.transfer(0xFF);
  const u8 frame[6] = {u8(0x40 | cmd), u8(arg >> 24), u8(arg >> 16), u8(arg >> 8),
                       u8(arg), 0x95};
  for (size_t i = 0; i < 5; ++i) card.transfer(frame[i]);
  // The card does not drive MISO during the command frame, so the CRC clock
  // reads high and the R1 arrives on the clock after it, exactly as it does on
  // a real card.
  card.transfer(frame[5]);
  out.bytes.push_back(card.transfer(0xFF));
  // Drain the rest: the queue is finite, so a fixed number of clocks is enough
  // and keeps the test independent of the internal length.
  for (size_t i = 0; i < 32 + extra; ++i) out.bytes.push_back(card.transfer(0xFF));
  return out;
}

bool ready(SdCard& card) {
  // The handshake every driver performs: CMD0, CMD8, CMD55, ACMD41 with HCS.
  command(card, 0, 0);
  command(card, 8, 0x1AA);
  command(card, 55, 0);
  const Exchange acmd41 = command(card, 41, 0x40000000);
  return !acmd41.bytes.empty() && acmd41.bytes[0] == 0x00;
}

// The SPI bus as a Next driver sees it: two ports, one latch and one shift
// register. Every access below goes through the machine, so the port decode is
// covered along with the card.
struct SpiBus {
  Machine& machine;

  void select(u8 value) { machine.ioWrite(0xE7, value, 0); }
  void write(u8 value) { machine.ioWrite(0xEB, value, 0); }
  u8 read() { return machine.ioRead(0xEB, 0); }

  void frame(u8 cmd, u32 arg) {
    // The card only leaves its response state on a clock, so a driver always
    // wastes a few before starting the next frame. Without them the first byte
    // of the command would be swallowed by the state transition.
    for (int i = 0; i < 8; ++i) write(0xFF);
    write(u8(0x40 | cmd));
    write(u8(arg >> 24));
    write(u8(arg >> 16));
    write(u8(arg >> 8));
    write(u8(arg));
    write(0x95);
  }

  // Clocks until the card answers with something other than 0xFF, which is
  // exactly how a driver waits out the one-byte pipeline delay.
  u8 response(int clocks = 16) {
    for (int i = 0; i < clocks; ++i) {
      const u8 value = read();
      if (value != 0xFF) return value;
    }
    return 0xFF;
  }

  bool init() {
    select(0xFE);
    for (int i = 0; i < 10; ++i) write(0xFF);
    frame(0, 0);
    if (response() != 0x01) return false;
    frame(8, 0x1AA);
    if (response() != 0x01) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
      frame(55, 0);
      response();
      frame(41, 0x40000000);
      if (response() == 0x00) return true;
    }
    return false;
  }

  // Reads one block and returns its 512 bytes.
  std::vector<u8> readBlock(u32 lba) {
    frame(17, lba);
    response();
    if (response() != 0xFE) return {};
    std::vector<u8> out;
    out.reserve(512);
    for (int i = 0; i < 512; ++i) out.push_back(read());
    return out;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

TEST(sdcard_rejects_images_that_are_not_whole_sectors) {
  SdCard card;
  std::string error;

  CHECK(!card.load({}, error));
  CHECK(!error.empty());
  CHECK(!card.inserted());

  error.clear();
  CHECK(!card.load(std::vector<u8>(700, 0), error));
  CHECK(error.find("512") != std::string::npos);
  CHECK(!card.inserted());

  error.clear();
  CHECK(card.load(makeCard(4), error));
  CHECK(error.empty());
  CHECK(card.inserted());
  CHECK_EQ(card.sectors(), 4);
}

TEST(sdcard_is_silent_until_it_is_selected) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));

  CHECK(!card.selected());
  CHECK_EQ(card.transfer(0x40), u8(0xFF));

  card.select(true);
  CHECK(card.selected());
  CHECK_EQ(card.transfer(0x40), u8(0xFF));
}

TEST(sdcard_answers_cmd0_and_cmd8) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));
  card.select(true);

  const Exchange goIdle = command(card, 0, 0);
  CHECK_EQ(goIdle.bytes[0], u8(0x01));

  const Exchange ifCond = command(card, 8, 0x1AA);
  CHECK_EQ(ifCond.bytes[0], u8(0x01));
  CHECK_EQ(ifCond.bytes[1], u8(0x00));
  CHECK_EQ(ifCond.bytes[2], u8(0x00));
  CHECK_EQ(ifCond.bytes[3], u8(0x01));
  CHECK_EQ(ifCond.bytes[4], u8(0xAA));
}

TEST(sdcard_initialises_through_acmd41) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));
  card.select(true);

  command(card, 0, 0);
  command(card, 8, 0x1AA);

  // ACMD41 without the CMD55 prefix is not a valid command.
  CHECK_EQ(command(card, 41, 0x40000000).bytes[0], u8(0x04));

  const Exchange app = command(card, 55, 0);
  CHECK_EQ(app.bytes[0], u8(0x01));

  const Exchange acmd41 = command(card, 41, 0x40000000);
  CHECK_EQ(acmd41.bytes[0], u8(0x00));

  const Exchange ocr = command(card, 58, 0);
  CHECK_EQ(ocr.bytes[0], u8(0x00));
  // Bit 7 is the power-up status and bit 6 is CCS: a ready SDHC card reports
  // both, and a driver that needs them together sees an SDSC card otherwise.
  CHECK_EQ(ocr.bytes[1], u8(0xC0));
  CHECK_EQ(ocr.bytes[2], u8(0xFF));
}

TEST(sdcard_reads_a_single_block) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));
  card.select(true);
  CHECK(ready(card));

  // SDHC addresses in sectors, so sector 2 starts at byte 1024.
  const Exchange read = command(card, 17, 2, 512);
  CHECK_EQ(read.bytes[0], u8(0x00));
  CHECK_EQ(read.bytes[1], u8(0xFF));

  // The data token is somewhere after the response byte, and the payload
  // follows it. Find the token rather than assuming an offset.
  size_t token = 0;
  while (token < read.bytes.size() && read.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 + 512 <= read.bytes.size());
  if (token + 1 + 512 <= read.bytes.size()) {
    CHECK_EQ(read.bytes[token + 1], u8(0x00));  // byte 1024 & 0xFF
    CHECK_EQ(read.bytes[token + 2], u8(0x01));
    CHECK_EQ(read.bytes[token + 512], u8(0xFF));  // byte 1535 & 0xFF
  }
}

TEST(sdcard_rejects_a_read_past_the_end) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(4), error));
  card.select(true);
  CHECK(ready(card));

  const Exchange read = command(card, 17, 4);
  CHECK_EQ(read.bytes[0], u8(0x40));  // parameter error
}

TEST(sdcard_writes_a_single_block) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(8), error));
  card.select(true);
  CHECK(ready(card));

  const Exchange start = command(card, 24, 1);
  CHECK_EQ(start.bytes[0], u8(0x00));

  // The card is now waiting for the data token, then 512 payload bytes and two
  // CRC bytes it ignores.
  CHECK_EQ(card.transfer(0x00), u8(0xFF));
  CHECK_EQ(card.transfer(0xFE), u8(0xFF));
  for (int i = 0; i < 512; ++i) card.transfer(u8(0xA0 + (i & 0x0F)));
  card.transfer(0xFF);  // first CRC byte
  // The response arrives on the clock of the second CRC byte.
  CHECK_EQ(card.transfer(0xFF), u8(0x05));  // data accepted


  const Exchange read = command(card, 17, 1, 512);
  size_t token = 0;
  while (token < read.bytes.size() && read.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 < read.bytes.size());
  if (token + 1 < read.bytes.size()) CHECK_EQ(read.bytes[token + 1], u8(0xA0));
}

TEST(sdcard_reads_multiple_blocks_until_cmd12) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeSectorCard(), error));
  card.select(true);
  CHECK(ready(card));

  // A driver announces the length first; the card only records that it may
  // happen, so the answer is a plain R1.
  CHECK_EQ(command(card, 23, 2).bytes[0], u8(0x00));

  // CMD18 has no length: the response is followed by block after block, each
  // one a token, 512 bytes and two CRC bytes.
  const Exchange read = command(card, 18, 2, 1024);
  CHECK_EQ(read.bytes[0], u8(0x00));

  constexpr size_t BlockBytes = 1 + 512 + 2;
  // R1, one byte of latency, then two blocks.
  CHECK(read.bytes.size() >= 2 + 2 * BlockBytes);
  if (read.bytes.size() >= 2 + 2 * BlockBytes) {
    CHECK_EQ(read.bytes[2], u8(0xFE));
    CHECK_EQ(read.bytes[3], u8(0x42));  // sector 2
    CHECK_EQ(read.bytes[2 + BlockBytes], u8(0xFE));
    CHECK_EQ(read.bytes[2 + BlockBytes + 1], u8(0x43));  // sector 3
  }

  // CMD12 interrupts the stream, and the card answers normally afterwards.
  const Exchange stop = command(card, 12, 0);
  CHECK_EQ(stop.bytes[0], u8(0x00));

  const Exchange single = command(card, 17, 7, 512);
  size_t token = 0;
  while (token < single.bytes.size() && single.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 < single.bytes.size());
  if (token + 1 < single.bytes.size()) CHECK_EQ(single.bytes[token + 1], u8(0x47));
}

TEST(sdcard_writes_multiple_blocks_until_the_stop_token) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(16), error));
  card.select(true);
  CHECK(ready(card));

  const Exchange start = command(card, 25, 4);
  CHECK_EQ(start.bytes[0], u8(0x00));

  // Each block is the data token, 512 bytes and two CRC bytes. The card answers
  // with the data-response token on the clock of the second CRC byte, then
  // spends two busy bytes before it listens again.
  for (int block = 0; block < 2; ++block) {
    for (int i = 0; i < 3; ++i) card.transfer(0xFF);
    CHECK_EQ(card.transfer(0xFE), u8(0xFF));
    for (int i = 0; i < 512; ++i) card.transfer(u8(0x11 * (block + 1)));
    card.transfer(0xFF);  // first CRC byte
    CHECK_EQ(card.transfer(0xFF), u8(0x05));  // data accepted
  }

  // CMD25 ends with the stop token instead of a length.
  for (int i = 0; i < 3; ++i) card.transfer(0xFF);
  CHECK_EQ(card.transfer(0xFD), u8(0x05));

  const Exchange first = command(card, 17, 4, 512);
  size_t token = 0;
  while (token < first.bytes.size() && first.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 < first.bytes.size());
  if (token + 1 < first.bytes.size()) CHECK_EQ(first.bytes[token + 1], u8(0x11));

  const Exchange second = command(card, 17, 5, 512);
  token = 0;
  while (token < second.bytes.size() && second.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 < second.bytes.size());
  if (token + 1 < second.bytes.size()) CHECK_EQ(second.bytes[token + 1], u8(0x22));
}

TEST(sdcard_reports_an_unknown_command) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));
  card.select(true);

  CHECK_EQ(command(card, 33, 0).bytes[0], u8(0x04));
}

TEST(sdcard_reports_its_size_in_the_csd) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(4096), error));  // 2 MB, enough for a non-zero C_SIZE
  card.select(true);
  CHECK(ready(card));

  const Exchange csd = command(card, 9, 0);
  size_t token = 0;
  while (token < csd.bytes.size() && csd.bytes[token] != 0xFE) ++token;
  CHECK(token + 1 + 16 <= csd.bytes.size());
  if (token + 1 + 16 <= csd.bytes.size()) {
    const u32 csize = (u32(csd.bytes[token + 8] & 0x3F) << 16) |
                      (u32(csd.bytes[token + 9]) << 8) | csd.bytes[token + 10];
    // 4096 sectors / 1024 - 1 = 3.
    CHECK_EQ(csize, u32(3));
  }
}

TEST(sdcard_deselect_abandons_a_write_in_flight) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(8), error));
  card.select(true);
  CHECK(ready(card));

  command(card, 24, 2);
  card.transfer(0xFE);
  card.transfer(0x11);
  card.select(false);
  card.select(true);

  // The half-written payload must not have reached the image.
  const Exchange read = command(card, 17, 2, 512);
  size_t token = 0;
  while (token < read.bytes.size() && read.bytes[token] != 0xFE) ++token;
  if (token + 1 < read.bytes.size()) CHECK_EQ(read.bytes[token + 1], u8(0x00));
}

TEST(sdcard_eject_empties_the_card) {
  SdCard card;
  std::string error;
  CHECK(card.load(makeCard(), error));
  card.select(true);
  CHECK(ready(card));

  card.eject();
  CHECK(!card.inserted());
  CHECK_EQ(card.sectors(), u32(0));
  CHECK_EQ(card.transfer(0x40), u8(0xFF));
}

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------
// The Next's SPI bus
// ---------------------------------------------------------------------------

TEST(next_spi_port_e7_selects_the_card_and_everything_else_releases_it) {
  Machine machine(MachineType::Next);
  std::string error;
  CHECK(machine.sdCard().load(makeCard(), error));
  SpiBus bus{machine};

  // No bit pulled low: nothing on the bus, so the card does not drive MISO.
  bus.select(0xFF);
  CHECK_EQ(bus.read(), u8(0xFF));
  // Card 0 and card 1 both reach the one card we emulate.
  bus.select(0xFE);
  CHECK(bus.init());
  bus.select(0xFF);
  CHECK_EQ(bus.read(), u8(0xFF));
  bus.select(0xFD);
  bus.frame(0, 0);
  CHECK_EQ(bus.response(), u8(0x01)); // an R1, so card 1 reached it too
  // The Pi's own SPI sockets must not reach the SD card.
  bus.select(0xFB);
  bus.frame(0, 0);
  CHECK_EQ(bus.response(), u8(0xFF));
}

TEST(next_spi_port_e7_reads_back_the_latch) {
  Machine machine(MachineType::Next);
  machine.ioWrite(0xE7, 0xFE, 0);
  CHECK_EQ(machine.ioRead(0xE7, 0), u8(0xFE));
  machine.ioWrite(0xE7, 0xFD, 0);
  CHECK_EQ(machine.ioRead(0xE7, 0), u8(0xFD));
}

TEST(next_spi_reads_a_block_through_the_data_port) {
  Machine machine(MachineType::Next);
  std::string error;
  const std::vector<u8> image = makeSectorCard(8);
  CHECK(machine.sdCard().load(image, error));
  SpiBus bus{machine};

  CHECK(bus.init());
  const std::vector<u8> block = bus.readBlock(3);
  CHECK_EQ(block.size(), size_t(512));
  for (int i = 0; i < 512; ++i) {
    if (block[size_t(i)] != u8(0x40 + 3)) {
      CHECK_EQ(block[size_t(i)], u8(0x40 + 3));
      break;
    }
  }
}

TEST(next_spi_answers_nothing_without_a_card) {
  Machine machine(MachineType::Next);
  SpiBus bus{machine};
  bus.select(0xFE);
  bus.frame(0, 0);
  // An empty socket shifts back 0xFF for ever rather than an R1.
  CHECK_EQ(bus.response(), u8(0xFF));
}

TEST(next_spi_bus_is_absent_on_a_classic_machine) {
  // 0xE7 and 0xEB are Next ports. A 48K has nothing there, and must not start
  // clocking the card because a program happened to write an odd port.
  Machine machine(MachineType::Spectrum48);
  std::string error;
  CHECK(machine.sdCard().load(makeCard(), error));
  machine.ioWrite(0xE7, 0xFE, 0);
  machine.ioWrite(0xEB, 0x40, 0);
  CHECK_EQ(machine.ioRead(0xEB, 0), u8(0xFF));
}

TEST(next_sd_card_survives_a_reset) {
  Machine machine(MachineType::Next);
  std::string error;
  CHECK(machine.sdCard().load(makeCard(16), error));
  CHECK_EQ(machine.sdCard().sectors(), size_t(16));
  machine.reset();
  // The image is the physical card, so a reset cannot take it out; only the
  // protocol restarts, which is why the handshake has to run again.
  CHECK_EQ(machine.sdCard().sectors(), size_t(16));
  CHECK(machine.sdCard().inserted());
  SpiBus bus{machine};
  // The reset dropped chip select, so the driver has to assert it again -- and
  // once it does, the card answers exactly as it did before.
  bus.frame(0, 0);
  CHECK_EQ(bus.response(), u8(0xFF));
  bus.select(0xFE);
  bus.frame(0, 0);
  CHECK_EQ(bus.response(), u8(0x01));
}
