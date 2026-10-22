#include "test_framework.h"

#include <vector>

#include "formats/sna.h"
#include "formats/snapshot.h"
#include "formats/z80file.h"

using rainbow::u8;
using rainbow::u16;
using rainbow::formats::Snapshot;

namespace {

void putWord(std::vector<u8>& v, size_t offset, u16 value) {
  v[offset] = u8(value & 0xFF);
  v[offset + 1] = u8(value >> 8);
}

void appendWord(std::vector<u8>& v, u16 value) {
  v.push_back(u8(value & 0xFF));
  v.push_back(u8(value >> 8));
}

// 64 runs of 255 bytes plus one run of 64 bytes fills a 16K bank with zeros.
std::vector<u8> rleZeros16k() {
  std::vector<u8> block;
  for (int i = 0; i < 64; ++i) {
    block.push_back(0xED);
    block.push_back(0xED);
    block.push_back(0xFF);
    block.push_back(0x00);
  }
  block.push_back(0xED);
  block.push_back(0xED);
  block.push_back(0x40);
  block.push_back(0x00);
  return block;
}

} // namespace

TEST(sna_48k) {
  std::vector<u8> sna(27 + 49152, 0);
  sna[0] = 0x3F;                        // I
  sna[9] = 0x34; sna[10] = 0x12;        // HL
  sna[13] = 0x78; sna[14] = 0x56;       // BC
  sna[21] = 0x00; sna[22] = 0x80;       // AF
  sna[23] = 0x00; sna[24] = 0xFF;       // SP
  sna[25] = 1;                          // IM 1
  sna[26] = 3;                          // border

  sna[27] = 0x11;                       // bank 5, first byte
  sna[27 + 16384] = 0x22;               // bank 2
  sna[27 + 32768] = 0x33;               // bank 0

  putWord(sna, 27 + 0xBF00, 0x1234);    // return address for PC

  Snapshot snap;
  std::string error;
  CHECK(rainbow::formats::loadSna(sna, snap, error));
  CHECK_EQ(snap.regs.pc, 0x1234);
  CHECK_EQ(snap.regs.sp, 0xFF02);
  CHECK_EQ(snap.regs.i, 0x3F);
  CHECK_EQ(snap.regs.im, 1);
  CHECK_EQ(snap.border, 3);
  CHECK(!snap.is128);
  CHECK_EQ(u16(snap.regs.h) << 8 | snap.regs.l, 0x1234);  CHECK_EQ(snap.ram[5 * 16384], 0x11);
  CHECK_EQ(snap.ram[2 * 16384], 0x22);
  CHECK_EQ(snap.ram[0], 0x33);
}

TEST(sna_128k) {
  std::vector<u8> sna(27 + 49152 + 4 + 5 * 16384, 0);
  sna[23] = 0x00; sna[24] = 0xFF;

  const size_t tail = 27 + 49152;
  putWord(sna, tail, 0x8000);      // PC
  sna[tail + 2] = 0x17;            // 7FFD

  static const int Order[5] = {1, 3, 4, 6, 7};
  for (int i = 0; i < 5; ++i) sna[tail + 4 + size_t(i) * 16384] = u8(0xA0 + Order[i]);

  Snapshot snap;
  std::string error;
  CHECK(rainbow::formats::loadSna(sna, snap, error));
  CHECK(snap.is128);
  CHECK_EQ(snap.regs.pc, 0x8000);
  CHECK_EQ(snap.port7ffd, 0x17);
  for (int i = 0; i < 5; ++i) {
    CHECK_EQ(snap.ram[size_t(Order[i]) * 16384], u8(0xA0 + Order[i]));
  }
}

TEST(sna_rejects_truncated) {
  std::vector<u8> sna(27 + 1024, 0);
  Snapshot snap;
  std::string error;
  CHECK(!rainbow::formats::loadSna(sna, snap, error));
  CHECK(!error.empty());
}

TEST(z80_v1) {
  std::vector<u8> z80(30 + 49152, 0);
  z80[0] = 0x00; z80[1] = 0x80;   // AF
  putWord(z80, 6, 0x8000);        // PC != 0 selects version 1
  putWord(z80, 8, 0xFF00);        // SP
  z80[12] = 0x07;                 // border 3, R bit 7 set
  z80[30] = 0x11;                 // bank 5

  Snapshot snap;
  std::string error;
  CHECK(rainbow::formats::loadZ80(z80, snap, error));
  CHECK_EQ(snap.regs.pc, 0x8000);
  CHECK_EQ(snap.regs.sp, 0xFF00);
  CHECK_EQ(u16(snap.regs.a) << 8 | snap.regs.f, 0x8000);
  CHECK_EQ(snap.border, 3);
  CHECK_EQ(snap.regs.r, 0x80);
  CHECK(!snap.is128);
  CHECK_EQ(snap.ram[5 * 16384], 0x11);
}

TEST(z80_v2_mixed_blocks) {
  std::vector<u8> z80(30, 0);
  putWord(z80, 6, 0x0000); // PC == 0 selects version 2/3
  putWord(z80, 8, 0xFF00);
  z80.resize(32, 0);
  putWord(z80, 30, 23);    // extended header length
  z80.resize(30 + 23, 0);
  putWord(z80, 32, 0x8000); // PC
  z80[34] = 0;              // hardware: 48K
  z80[35] = 0x00;           // 7FFD

  // Bank 5: RLE-compressed zeros.
  const std::vector<u8> compressed = rleZeros16k();
  appendWord(z80, u16(compressed.size()));
  z80.insert(z80.end(), compressed.begin(), compressed.end());

  // Bank 2: uncompressed, with a marker at the first byte.
  appendWord(z80, 0xFFFF);
  std::vector<u8> plain(16384, 0);
  plain[0] = 0x11;
  z80.insert(z80.end(), plain.begin(), plain.end());

  // Bank 0: uncompressed.
  appendWord(z80, 0xFFFF);
  z80.insert(z80.end(), plain.begin(), plain.end());

  Snapshot snap;
  std::string error;
  CHECK(rainbow::formats::loadZ80(z80, snap, error));
  CHECK_EQ(snap.regs.pc, 0x8000);
  CHECK_EQ(snap.regs.sp, 0xFF00);
  CHECK(!snap.is128);
  CHECK_EQ(snap.ram[5 * 16384], 0x00);
  CHECK_EQ(snap.ram[5 * 16384 + 16383], 0x00);
  CHECK_EQ(snap.ram[2 * 16384], 0x11);
}

TEST(z80_rejects_truncated) {
  std::vector<u8> z80(20, 0);
  Snapshot snap;
  std::string error;
  CHECK(!rainbow::formats::loadZ80(z80, snap, error));
  CHECK(!error.empty());
}
