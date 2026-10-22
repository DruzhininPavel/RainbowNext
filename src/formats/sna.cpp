#include "formats/sna.h"

#include <cstring>

namespace rainbow::formats {
namespace {

constexpr size_t SnaHeaderSize = 27;
constexpr size_t BankSize = 16384;
constexpr size_t Ram48Size = 48 * 1024;

u16 word(const std::vector<u8>& d, size_t offset) {
  return u16(d[offset] | (u16(d[offset + 1]) << 8));
}

} // namespace

bool loadSna(const std::vector<u8>& data, Snapshot& out, std::string& error) {
  if (data.size() < SnaHeaderSize + Ram48Size) {
    error = "SNA file is too small";
    return false;
  }

  out.ram.assign(8 * BankSize, 0);

  // Header
  out.regs.i = data[0];
  out.regs.l2 = data[1];
  out.regs.h2 = data[2];
  out.regs.e2 = data[3];
  out.regs.d2 = data[4];
  out.regs.c2 = data[5];
  out.regs.b2 = data[6];
  out.regs.f2 = data[7];
  out.regs.a2 = data[8];
  out.regs.l = data[9];
  out.regs.h = data[10];
  out.regs.e = data[11];
  out.regs.d = data[12];
  out.regs.c = data[13];
  out.regs.b = data[14];
  out.regs.iy = word(data, 15);
  out.regs.ix = word(data, 17);
  out.regs.iff2 = (data[19] & 0x04) != 0;
  out.regs.iff1 = out.regs.iff2;
  out.regs.r = data[20];
  out.regs.f = data[21];
  out.regs.a = data[22];
  out.regs.sp = word(data, 23);
  out.regs.im = u8(data[25] & 0x03);
  out.border = u8(data[26] & 0x07);

  // The first 48K always maps to banks 5, 2, 0.
  const u8* ram48 = data.data() + SnaHeaderSize;
  std::memcpy(out.ram.data() + 5 * BankSize, ram48, BankSize);
  std::memcpy(out.ram.data() + 2 * BankSize, ram48 + BankSize, BankSize);
  std::memcpy(out.ram.data() + 0 * BankSize, ram48 + 2 * BankSize, BankSize);

  const bool is128 = data.size() >= SnaHeaderSize + Ram48Size + 4 + 5 * BankSize;
  if (!is128) {
    out.is128 = false;
    out.port7ffd = 0;
    // In a 48K SNA the PC was pushed on the stack before the snapshot.
    const size_t spOffset = out.regs.sp - 0x4000;
    if (spOffset + 1 >= Ram48Size) {
      error = "SNA stack pointer is outside RAM";
      return false;
    }
    out.regs.pc = u16(ram48[spOffset] | (u16(ram48[spOffset + 1]) << 8));
    out.regs.sp = u16(out.regs.sp + 2);
    return true;
  }

  out.is128 = true;
  const size_t tail = SnaHeaderSize + Ram48Size;
  out.regs.pc = word(data, tail);
  out.port7ffd = data[tail + 2];
  // tail + 3 is the TR-DOS ROM flag; ignored for now.

  static constexpr int BankOrder[5] = {1, 3, 4, 6, 7};
  const u8* p = data.data() + tail + 4;
  const size_t available = data.size() - (tail + 4);
  for (int i = 0; i < 5; ++i) {
    const size_t offset = size_t(i) * BankSize;
    if (offset + BankSize > available) break;
    std::memcpy(out.ram.data() + size_t(BankOrder[i]) * BankSize, p + offset, BankSize);
  }
  return true;
}

} // namespace rainbow::formats
