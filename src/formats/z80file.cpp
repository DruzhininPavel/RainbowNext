#include "formats/z80file.h"

#include <cstring>

namespace mynext::formats {
namespace {

constexpr size_t HeaderSize = 30;
constexpr size_t BankSize = 16384;
constexpr size_t Ram48Size = 48 * 1024;

u16 word(const std::vector<u8>& d, size_t offset) {
  return u16(d[offset] | (u16(d[offset + 1]) << 8));
}

void loadHeader(const std::vector<u8>& data, Snapshot& out) {
  out.regs.f = data[0];
  out.regs.a = data[1];
  out.regs.c = data[2];
  out.regs.b = data[3];
  out.regs.l = data[4];
  out.regs.h = data[5];
  out.regs.sp = word(data, 8);
  out.regs.i = data[10];
  out.regs.r = u8((data[11] & 0x7F) | ((data[12] & 0x01) << 7));
  out.border = u8((data[12] >> 1) & 0x07);
  out.regs.e = data[13];
  out.regs.d = data[14];
  out.regs.c2 = data[15];
  out.regs.b2 = data[16];
  out.regs.e2 = data[17];
  out.regs.d2 = data[18];
  out.regs.l2 = data[19];
  out.regs.h2 = data[20];
  out.regs.a2 = data[21];
  out.regs.f2 = data[22];
  out.regs.iy = word(data, 23);
  out.regs.ix = word(data, 25);
  out.regs.iff1 = data[27] != 0;
  out.regs.iff2 = data[28] != 0;
  out.regs.im = u8(data[29] & 0x03);
}

bool decompressBlock(const std::vector<u8>& data, size_t& pos, size_t length,
                     std::vector<u8>& out) {
  const size_t end = pos + length;
  if (end > data.size()) return false;

  out.clear();
  out.reserve(BankSize);
  while (pos < end && out.size() < BankSize) {
    const u8 byte = data[pos++];
    if (byte == 0xED && pos < end && data[pos] == 0xED) {
      ++pos;
      if (pos + 1 >= end) return false;
      const u8 count = data[pos++];
      const u8 value = data[pos++];
      out.insert(out.end(), count, value);
    } else {
      out.push_back(byte);
    }
  }
  return out.size() == BankSize;
}

bool readBlock(const std::vector<u8>& data, size_t& pos, std::vector<u8>& out) {
  if (pos + 2 > data.size()) return false;
  const u16 length = word(data, pos);
  pos += 2;
  if (length == 0xFFFF) {
    if (pos + BankSize > data.size()) return false;
    out.assign(data.begin() + long(pos), data.begin() + long(pos + BankSize));
    pos += BankSize;
    return true;
  }
  return decompressBlock(data, pos, length, out);
}

} // namespace

bool loadZ80(const std::vector<u8>& data, Snapshot& out, std::string& error) {
  if (data.size() < HeaderSize) {
    error = "Z80 file is too small";
    return false;
  }

  out.ram.assign(8 * BankSize, 0);
  loadHeader(data, out);

  const u16 headerPc = word(data, 6);
  if (headerPc != 0) {
    // Version 1: uncompressed 48K image follows the header.
    if (data.size() < HeaderSize + Ram48Size) {
      error = "Z80 v1 file is truncated";
      return false;
    }
    out.is128 = false;
    out.port7ffd = 0;
    out.regs.pc = headerPc;
    const u8* ram48 = data.data() + HeaderSize;
    std::memcpy(out.ram.data() + 5 * BankSize, ram48, BankSize);
    std::memcpy(out.ram.data() + 2 * BankSize, ram48 + BankSize, BankSize);
    std::memcpy(out.ram.data() + 0 * BankSize, ram48 + 2 * BankSize, BankSize);
    return true;
  }

  // Version 2 / 3: extended header.
  if (data.size() < HeaderSize + 2) {
    error = "Z80 extended header is missing";
    return false;
  }
  const u16 extendedLength = word(data, HeaderSize);
  if (data.size() < HeaderSize + extendedLength) {
    error = "Z80 extended header is truncated";
    return false;
  }

  out.regs.pc = word(data, 32);
  const u8 hardware = data[34];
  out.port7ffd = data[35];
  out.is128 = hardware >= 3;

  size_t pos = HeaderSize + extendedLength;
  std::vector<u8> block;

  static constexpr int Order48[3] = {5, 2, 0};
  static constexpr int Order128[8] = {5, 2, 0, 1, 3, 4, 6, 7};
  const int* order = out.is128 ? Order128 : Order48;
  const int blocks = out.is128 ? 8 : 3;

  for (int i = 0; i < blocks; ++i) {
    if (!readBlock(data, pos, block)) {
      error = "Z80 memory block is corrupt or truncated";
      return false;
    }
    std::memcpy(out.ram.data() + size_t(order[i]) * BankSize, block.data(), BankSize);
  }
  return true;
}

} // namespace mynext::formats
