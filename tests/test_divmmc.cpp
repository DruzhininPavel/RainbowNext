#include "test_framework.h"

#include <string>
#include <vector>

#include "core/machine.h"
#include "peripherals/divmmc.h"

using rainbow::DivMmc;
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

// An 8K esxDOS stand-in: every byte is its own address, so a page mix-up shows
// up immediately as a wrong value.
std::vector<u8> makeRom(int pages = 1) {
  std::vector<u8> rom(size_t(pages) * DivMmc::RomSize);
  for (size_t i = 0; i < rom.size(); ++i) rom[i] = u8(i & 0xFF);
  return rom;
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
  // The R1 comes back on the clock that shifts the CRC byte in, exactly as it
  // does on a real card, so that transfer's answer starts the response.
  out.bytes.push_back(card.transfer(frame[5]));
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
  CHECK_EQ(ocr.bytes[1], u8(0x40));
  CHECK_EQ(ocr.bytes[2], u8(0xFF));  // the HCS bit: block addressing
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

TEST(divmmc_loads_an_8k_or_32k_rom) {
  DivMmc div;
  std::string error;

  CHECK(!div.loadRom({}, error));
  CHECK(!error.empty());

  error.clear();
  CHECK(!div.loadRom(std::vector<u8>(0x1000, 0), error));
  CHECK(error.find("8K") != std::string::npos);

  error.clear();
  CHECK(!div.loadRom(std::vector<u8>(5 * DivMmc::RomSize, 0), error));
  CHECK(!error.empty());

  error.clear();
  CHECK(div.loadRom(makeRom(1), error));
  CHECK(div.romLoaded());
  CHECK_EQ(div.romPages(), u32(1));

  error.clear();
  CHECK(div.loadRom(makeRom(4), error));
  CHECK_EQ(div.romPages(), u32(4));
}

TEST(divmmc_is_transparent_until_it_is_mapped_in) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  u8 value = 0;
  CHECK(!div.memRead(0x0066, 0, false, value));
  CHECK(!div.memWrite(0x2000, 0x12, 0));
  CHECK(!div.mappedIn());
}

TEST(divmmc_maps_in_on_an_m1_fetch_at_a_hook_address) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  u8 value = 0;
  // A plain read of the NMI vector must not map it in -- only the opcode fetch
  // does, which is exactly what the M1 flag is for.
  CHECK(!div.memRead(0x0066, 0, false, value));
  CHECK(!div.mappedIn());

  CHECK(div.memRead(0x0066, 0, true, value));
  CHECK(div.mappedIn());
  CHECK_EQ(div.control() & DivMmc::Conmem, u8(DivMmc::Conmem));
  CHECK_EQ(value, u8(0x66));
}

TEST(divmmc_ignores_m1_fetches_at_other_addresses) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  u8 value = 0;
  CHECK(!div.memRead(0x1234, 0, true, value));
  CHECK(!div.mappedIn());
}

TEST(divmmc_has_no_memory_to_offer_without_a_rom) {
  DivMmc div;
  u8 value = 0;
  CHECK(!div.memRead(0x0066, 0, true, value));
  CHECK(!div.mappedIn());
}

TEST(divmmc_shows_its_rom_over_the_bottom_8k) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));
  div.setControl(DivMmc::Conmem);

  u8 value = 0;
  CHECK(div.memRead(0x0000, 0, false, value));
  CHECK_EQ(value, u8(0x00));
  CHECK(div.memRead(0x1FFF, 0, false, value));
  CHECK_EQ(value, u8(0xFF));
  CHECK(div.memRead(0x1000, 0, false, value));
  CHECK_EQ(value, u8(0x00));

  // The ROM half is read-only but still belongs to the module, so a write is
  // claimed and dropped rather than falling through to the machine's RAM.
  CHECK(div.memWrite(0x1000, 0x5A, 0));
  CHECK(div.memRead(0x1000, 0, false, value));
  CHECK_EQ(value, u8(0x00));
}

TEST(divmmc_switches_rom_pages_of_a_32k_image) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(4), error));
  CHECK_EQ(div.romPages(), u32(4));

  u8 value = 0;
  // Bits 5:4 of the latch pick the page; page 1 starts at 0x2000 in the image.
  div.setControl(u8(DivMmc::Conmem | 0x10));
  CHECK_EQ(div.romPage(), u32(1));
  CHECK(div.memRead(0x0000, 0, false, value));
  CHECK_EQ(value, u8(0x00));
  CHECK(div.memRead(0x0010, 0, false, value));
  CHECK_EQ(value, u8(0x10));

  div.setControl(u8(DivMmc::Conmem | 0x20));
  CHECK_EQ(div.romPage(), u32(2));
}

TEST(divmmc_selects_its_ram_bank_through_the_control_port) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  // Mapping in through the port, then writing the RAM window.
  CHECK(div.ioWrite(DivMmc::PortMapIn, 0, 0));
  CHECK(div.mappedIn());
  CHECK_EQ(div.bank(), 0);
  CHECK(div.memWrite(0x2000, 0xA1, 0));

  // Bank 5: the same offset must come back different.
  CHECK(div.ioWrite(DivMmc::PortControl, u8(DivMmc::Conmem | 5), 0));
  CHECK_EQ(div.bank(), 5);
  u8 value = 0;
  CHECK(div.memRead(0x2000, 0, false, value));
  CHECK_EQ(value, u8(0x00));
  CHECK(div.memWrite(0x2000, 0xB5, 0));

  CHECK(div.ioWrite(DivMmc::PortControl, u8(DivMmc::Conmem), 0));
  CHECK(div.memRead(0x2000, 0, false, value));
  CHECK_EQ(value, u8(0xA1));

  CHECK(div.ioWrite(DivMmc::PortControl, u8(DivMmc::Conmem | 5), 0));
  CHECK(div.memRead(0x2000, 0, false, value));
  CHECK_EQ(value, u8(0xB5));
}

TEST(divmmc_mapram_puts_ram_bank_3_over_the_rom) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  div.setControl(DivMmc::Conmem);
  u8 value = 0;
  CHECK(div.memRead(0x0000, 0, false, value));
  CHECK_EQ(value, u8(0x00));

  // Bank 3 is the module's own RAM, so writing it needs mapram set.
  div.setControl(u8(DivMmc::Conmem | DivMmc::Mapram));
  CHECK(div.mapram());
  CHECK(div.memWrite(0x0100, 0x7E, 0));
  CHECK(div.memRead(0x0100, 0, false, value));
  CHECK_EQ(value, u8(0x7E));
  CHECK(div.memRead(0x0000, 0, false, value));
  CHECK_EQ(value, u8(0x00));

  // And it survives being mapped out and back, because it is the same bank.
  div.setControl(DivMmc::Conmem);
  CHECK(div.memRead(0x0100, 0, false, value));
  CHECK_EQ(value, u8(0x00));
}

TEST(divmmc_unmap_and_map_ports_drive_the_conmem_bit) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));

  CHECK(div.ioWrite(DivMmc::PortMapIn, 0x5A, 0));
  CHECK(div.mappedIn());

  // The value is irrelevant: both ports only touch bit 7.
  CHECK(div.ioWrite(DivMmc::PortUnmap, 0xFF, 0));
  CHECK(!div.mappedIn());
  CHECK_EQ(div.control() & 0x0F, u8(0));  // the bank is left alone
}

TEST(divmmc_reads_back_its_control_latch) {
  DivMmc div;
  u8 value = 0;
  CHECK(div.ioRead(DivMmc::PortControl, 0, value));
  CHECK_EQ(value, u8(0x00));

  CHECK(div.ioWrite(DivMmc::PortControl, u8(DivMmc::Conmem | DivMmc::Mapram | 7), 0));
  CHECK(div.ioRead(DivMmc::PortControl, 0, value));
  CHECK_EQ(value, u8(DivMmc::Conmem | DivMmc::Mapram | 7));
}

TEST(divmmc_status_port_reports_card_detect) {
  DivMmc div;
  u8 value = 0;

  CHECK(div.ioRead(DivMmc::PortStatus, 0, value));
  CHECK_EQ(value & 0x80, u8(0x80));  // no card

  std::string error;
  CHECK(div.loadCard(makeCard(), error));
  CHECK(div.ioRead(DivMmc::PortStatus, 0, value));
  CHECK_EQ(value & 0x80, u8(0x00));  // card present
}

TEST(divmmc_does_not_claim_ports_it_does_not_own) {
  DivMmc div;
  u8 value = 0;
  CHECK(!div.ioRead(0xFE, 0, value));
  CHECK(!div.ioWrite(0xFE, 0, 0));
  CHECK(!div.ioRead(0x1234, 0, value));
}

TEST(divmmc_talks_to_the_card_while_it_is_mapped_out) {
  DivMmc div;
  std::string error;
  CHECK(div.loadCard(makeCard(), error));

  // The board ties chip select to the mapping latch: unmapped means the card
  // is listening, which is why esxDOS unmaps before every SPI access.
  div.ioWrite(DivMmc::PortControl, 0, 0);
  CHECK(div.card().selected());

  div.ioWrite(DivMmc::PortMapIn, 0, 0);
  CHECK(!div.card().selected());

  div.ioWrite(DivMmc::PortUnmap, 0, 0);
  CHECK(div.card().selected());
}

TEST(divmmc_routes_spi_bytes_through_the_data_port) {
  DivMmc div;
  std::string error;
  CHECK(div.loadCard(makeCard(), error));
  div.ioWrite(DivMmc::PortUnmap, 0, 0);

  // CMD0 shifted out one byte at a time, then the R1 that comes back.
  div.ioWrite(DivMmc::PortSpiData, 0x40, 0);
  for (int i = 0; i < 4; ++i) div.ioWrite(DivMmc::PortSpiData, 0x00, 0);
  div.ioWrite(DivMmc::PortSpiData, 0x95, 0);

  u8 value = 0;
  CHECK(div.ioRead(DivMmc::PortSpiData, 0, value));
  CHECK_EQ(value, u8(0x01));  // idle
}

TEST(divmmc_next_selects_the_card_through_port_e7) {
  DivMmc div;
  div.setLegacySpiPort(false);
  std::string error;
  CHECK(div.loadCard(makeCard(), error));

  // On the Next the mapping latch says nothing about the card: every socket
  // stays released until 0xE7 picks one.
  CHECK(!div.card().selected());
  CHECK(div.ioWrite(DivMmc::PortControl, u8(DivMmc::Conmem), 0));
  CHECK(!div.card().selected());

  // Bit 0 low picks SD card 0 and bit 1 low picks card 1; the value is not
  // otherwise a chip select, so 0xFE and 0xFD both mean "a card is listening".
  CHECK(div.ioWrite(DivMmc::PortUnmap, 0xFE, 0));
  CHECK(div.card().selected());
  CHECK(div.ioWrite(DivMmc::PortUnmap, 0xFD, 0));
  CHECK(div.card().selected());

  // All bits high is the released state the driver leaves behind.
  CHECK(div.ioWrite(DivMmc::PortUnmap, 0xFF, 0));
  CHECK(!div.card().selected());
}

TEST(divmmc_next_data_port_reads_clock_the_card) {
  DivMmc div;
  div.setLegacySpiPort(false);
  std::string error;
  CHECK(div.loadCard(makeCard(), error));
  CHECK(div.ioWrite(DivMmc::PortUnmap, 0xFE, 0));

  // Everything goes through 0xEB, and only a read clocks a byte in: the driver
  // spins on the port until the card answers.
  auto send = [&div](u8 cmd, u32 arg) {
    div.ioWrite(DivMmc::PortData, u8(0x40 | cmd), 0);
    div.ioWrite(DivMmc::PortData, u8(arg >> 24), 0);
    div.ioWrite(DivMmc::PortData, u8(arg >> 16), 0);
    div.ioWrite(DivMmc::PortData, u8(arg >> 8), 0);
    div.ioWrite(DivMmc::PortData, u8(arg), 0);
    div.ioWrite(DivMmc::PortData, 0x95, 0);
  };
  auto poll = [&div](size_t count) {
    std::vector<u8> got;
    for (size_t i = 0; i < count; ++i) {
      u8 value = 0xFF;
      div.ioRead(DivMmc::PortData, 0, value);
      got.push_back(value);
    }
    return got;
  };

  poll(4);  // the card needs a few clocks to enter SPI mode
  send(0, 0);
  poll(4);
  send(8, 0x1AA);
  poll(4);
  send(55, 0);
  poll(4);
  send(41, 0x40000000);
  poll(4);

  send(17, 0);  // READ_SINGLE_BLOCK, sector 0
  const std::vector<u8> block = poll(64);
  bool sawToken = false;
  for (u8 value : block) {
    if (value == 0xFE) sawToken = true;
  }
  CHECK(sawToken);
}

TEST(divmmc_loads_an_image_by_extension) {
  DivMmc div;
  std::string error;

  CHECK(div.loadRom(makeRom(), error));
  CHECK(div.romLoaded());
  CHECK(!div.card().inserted());

  CHECK(div.loadCard(makeCard(), error));
  CHECK(div.card().inserted());
  CHECK_EQ(div.card().sectors(), u32(64));

  const std::string filter = div.fileFilter();
  CHECK(filter.find(".rom") != std::string::npos);
  CHECK(filter.find(".mmc") != std::string::npos);
}

TEST(divmmc_recognises_the_hook_addresses) {
  CHECK(DivMmc::isHookAddress(0x0000));
  CHECK(DivMmc::isHookAddress(0x0008));
  CHECK(DivMmc::isHookAddress(0x0038));
  CHECK(DivMmc::isHookAddress(0x0066));
  CHECK(DivMmc::isHookAddress(0x04C6));
  CHECK(DivMmc::isHookAddress(0x0562));
  CHECK(!DivMmc::isHookAddress(0x0001));
  CHECK(!DivMmc::isHookAddress(0x1000));
}

TEST(divmmc_disable_lets_go_of_the_card) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));
  div.setControl(DivMmc::Conmem);
  CHECK(div.mappedIn());

  div.disable();
  CHECK(!div.mappedIn());
  CHECK(!div.card().selected());

  u8 value = 0;
  CHECK(!div.memRead(0x0000, 0, false, value));
}

TEST(divmmc_reset_clears_the_latch_and_the_ram) {
  DivMmc div;
  std::string error;
  CHECK(div.loadRom(makeRom(), error));
  CHECK(div.loadCard(makeCard(), error));

  div.setControl(u8(DivMmc::Conmem | 2));
  div.memWrite(0x2000, 0x99, 0);
  div.reset();

  CHECK(!div.mappedIn());
  CHECK_EQ(div.bank(), 0);
  // The card is still in the socket after a reset; only its protocol state is
  // gone, and the image survives.
  CHECK(div.card().inserted());

  div.setControl(u8(DivMmc::Conmem | 2));
  u8 value = 0;
  CHECK(div.memRead(0x2000, 0, false, value));
  CHECK_EQ(value, u8(0x00));
}

// ---------------------------------------------------------------------------
// Through the machine
// ---------------------------------------------------------------------------

TEST(divmmc_maps_in_through_the_machine_on_an_m1_fetch) {
  Machine machine(MachineType::Spectrum48);
  std::vector<u8> system(0x4000, 0x3F);
  CHECK(machine.loadRomImageData(system.data(), system.size()));

  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);
  std::string error;
  CHECK(div->loadRom(std::vector<u8>(DivMmc::RomSize, 0xC9), error));
  CHECK(machine.expansion().setEnabled("divmmc", true));

  // A plain read of the hook address leaves the machine's own ROM alone: only
  // an opcode fetch is a call into the module.
  CHECK_EQ(machine.memRead(0x0066, 0), u8(0x3F));
  CHECK(!div->mappedIn());

  CHECK_EQ(machine.memFetch(0x0066, 0), u8(0xC9));
  CHECK(div->mappedIn());
  CHECK_EQ(machine.memRead(0x0066, 0), u8(0xC9));
  CHECK_EQ(machine.memRead(0x1000, 0), u8(0xC9));
}

TEST(divmmc_ram_window_hides_the_machine_rom) {
  Machine machine(MachineType::Spectrum48);
  std::vector<u8> system(0x4000, 0x3F);
  CHECK(machine.loadRomImageData(system.data(), system.size()));

  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);
  std::string error;
  CHECK(div->loadRom(std::vector<u8>(DivMmc::RomSize, 0x00), error));
  CHECK(machine.expansion().setEnabled("divmmc", true));

  // Nothing mapped in: the CPU still sees the machine's own ROM.
  CHECK_EQ(machine.memRead(0x2000, 0), u8(0x3F));

  div->setControl(DivMmc::Conmem);
  machine.memWrite(0x2000, 0x22, 0);
  CHECK_EQ(machine.memRead(0x2000, 0), u8(0x22));

  // Unmapping hands the ROM back, and the byte the module took is still in its
  // own RAM when it maps in again.
  div->setControl(0);
  CHECK_EQ(machine.memRead(0x2000, 0), u8(0x3F));
  div->setControl(DivMmc::Conmem);
  CHECK_EQ(machine.memRead(0x2000, 0), u8(0x22));
}

TEST(divmmc_is_off_the_bus_until_it_is_plugged_in) {
  Machine machine(MachineType::Spectrum48);
  std::vector<u8> system(0x4000, 0x3F);
  CHECK(machine.loadRomImageData(system.data(), system.size()));

  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);
  std::string error;
  CHECK(div->loadRom(std::vector<u8>(DivMmc::RomSize, 0xC9), error));

  CHECK(!machine.expansion().enabled("divmmc"));
  CHECK_EQ(machine.memFetch(0x0066, 0), u8(0x3F));
  CHECK(!div->mappedIn());
}

TEST(divmmc_hook_enable_follows_the_entry_point_registers) {
  DivMmc div;
  div.setEntryPoints(0x82, 0x00, 0x00, 0xCD);

  // 0xB8 asks for 0x0038 and 0x0008 and nothing else, so the reset vector is
  // not an entry point any more.
  CHECK(!div.hookEnabled(0x0000));
  CHECK(div.hookEnabled(0x0038));
  CHECK(!div.hookEnabled(0x0010));

  // 0xB9 is clear for 0x0008, so the point only fires while the +3 DOS ROM is
  // the one paged in. That is exactly what keeps the Next boot ROM's own use of
  // RST 8 as a bank switcher working.
  bool rom3 = false;
  div.setRom3PresentProvider([&rom3] { return rom3; });
  CHECK(!div.hookEnabled(0x0008));
  rom3 = true;
  CHECK(div.hookEnabled(0x0008));

  // 0xB9 bit 0 is set for 0x0000, so that point does not care about ROM3.
  CHECK(div.hookEnabled(0x0038));

  // 0xBB keeps the esxDOS hooks and drops the NMI vectors, which it does by
  // leaving bits 0 and 1 clear.
  div.setEntryPoints(0x00, 0x00, 0x00, 0x04);
  CHECK(!div.hookEnabled(0x0066));
  CHECK(div.hookEnabled(0x04C6));
  CHECK(!div.hookEnabled(0x0562));
}

TEST(next_divmmc_automap_starts_off_and_follows_register_0x0a) {
  Machine machine(MachineType::Next);
  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);

  // A hard reset clears the master switch, so the boot ROM keeps both RST
  // vectors for itself.
  CHECK(!div->automap());

  machine.writeNextReg(0x0A, 0x10);
  CHECK(div->automap());
  machine.writeNextReg(0x0A, 0x00);
  CHECK(!div->automap());
}

TEST(next_divmmc_entry_point_registers_reach_the_module) {
  Machine machine(MachineType::Next);
  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);

  // The soft reset defaults: 0x0000 and 0x0038 everywhere, the rest only with
  // ROM3 paged in.
  CHECK_EQ(div->entryPoints0(), u8(0x83));
  CHECK_EQ(div->entryPointsValid0(), u8(0x01));
  CHECK_EQ(div->entryPointsTiming0(), u8(0x00));
  CHECK_EQ(div->entryPoints1(), u8(0xCD));

  machine.writeNextReg(0xB8, 0x82);
  machine.writeNextReg(0xB9, 0x00);
  machine.writeNextReg(0xBB, 0xF2);
  CHECK_EQ(div->entryPoints0(), u8(0x82));
  CHECK_EQ(div->entryPointsValid0(), u8(0x00));
  CHECK_EQ(div->entryPoints1(), u8(0xF2));
}

TEST(next_divmmc_rst8_only_maps_in_with_rom3_paged) {
  Machine machine(MachineType::Next);
  std::vector<u8> system(0x10000, 0x3F);
  CHECK(machine.loadRomImageData(system.data(), system.size()));
  machine.reset();

  auto* div = static_cast<DivMmc*>(machine.expansion().find("divmmc"));
  CHECK(div != nullptr);
  std::string error;
  CHECK(div->loadRom(std::vector<u8>(DivMmc::RomSize, 0xC9), error));

  // What the Next boot ROM itself programs before it turns the automap on.
  machine.writeNextReg(0x0A, 0x10);
  machine.writeNextReg(0xB8, 0x82);
  machine.writeNextReg(0xB9, 0x00);

  // ROM bank 0 is paged, so the RST 8 vector belongs to the ROM.
  CHECK_EQ(machine.romBank(), u8(0x00));
  CHECK_EQ(machine.memFetch(0x0008, 0), u8(0x3F));
  CHECK(!div->mappedIn());

  // Page the +3 DOS ROM in and the same fetch is a call into esxDOS. ROM 3 is
  // the one that needs both select bits: 0x7FFD bit 4 for the low bit and
  // 0x1FFD bit 2 for the high one.
  machine.ioWrite(0x7FFD, 0x10, 0);
  machine.ioWrite(0x1FFD, 0x04, 0);
  CHECK_EQ(machine.romBank(), u8(0x03));
  CHECK_EQ(machine.memFetch(0x0008, 0), u8(0xC9));
  CHECK(div->mappedIn());
}
