#include "formats/tape.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace rainbow::formats {
namespace {

// 3.5 MHz: the unit every tape timing in this file is expressed in.
constexpr u32 TstatesPerMs = 3500;

// A malformed or hostile file must not be able to expand into an unbounded
// block list; TZX loops are resolved at parse time, so they need a ceiling.
constexpr size_t MaxBlocks = 200000;

const u8 TzxMagic[8] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A};

bool truncated(std::string& error) {
  error = "tape file ends in the middle of a block";
  return false;
}

class Reader {
public:
  explicit Reader(const std::vector<u8>& data) : data_(data) {}

  size_t pos() const { return pos_; }
  void seek(size_t pos) { pos_ = pos; }
  size_t size() const { return data_.size(); }
  bool need(size_t n) const { return pos_ + n <= data_.size(); }

  u8 byte() { return data_[pos_++]; }
  u16 word() {
    const u16 v = u16(data_[pos_] | (u16(data_[pos_ + 1]) << 8));
    pos_ += 2;
    return v;
  }
  u32 triple() {
    const u32 v = u32(data_[pos_]) | (u32(data_[pos_ + 1]) << 8) | (u32(data_[pos_ + 2]) << 16);
    pos_ += 3;
    return v;
  }
  u32 quad() {
    const u32 v = u32(data_[pos_]) | (u32(data_[pos_ + 1]) << 8) |
                  (u32(data_[pos_ + 2]) << 16) | (u32(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
  }
  std::vector<u8> bytes(size_t n) {
    std::vector<u8> out(data_.begin() + long(pos_), data_.begin() + long(pos_ + n));
    pos_ += n;
    return out;
  }
  std::string text(size_t n) {
    std::string out(reinterpret_cast<const char*>(data_.data() + pos_), n);
    pos_ += n;
    return out;
  }
  void skip(size_t n) { pos_ += n; }

private:
  const std::vector<u8>& data_;
  size_t pos_ = 0;
};

TapeBlock standardData(std::vector<u8> payload) {
  TapeBlock block;
  block.kind = TapeBlock::Kind::Data;
  block.data = std::move(payload);
  block.pilotPulses = StandardPilotPulses;
  block.pilotLength = StandardPilotLength;
  block.syncFirst = StandardSyncFirst;
  block.syncSecond = StandardSyncSecond;
  block.zeroLength = StandardZeroLength;
  block.oneLength = StandardOneLength;
  block.tailLength = StandardTailLength;
  block.pauseMs = StandardPauseMs;
  return block;
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

} // namespace

size_t TapeBlock::bitCount() const {
  if (data.empty()) return 0;
  const u32 used = usedBitsLastByte == 0 ? 8 : usedBitsLastByte;
  return data.size() * 8 - (8 - std::min<u32>(used, 8));
}

u64 TapeBlock::pulseCount() const {
  switch (kind) {
    case Kind::Data: return u64(pilotPulses) + 2 + u64(bitCount()) * 2 + 1;
    case Kind::Pulses: return u64(pulseLengths.size()) * pulseRepeat;
    case Kind::Direct: return sampleBits;
    case Kind::Pause: return 1;
  }
  return 0;
}

u32 TapeBlock::pulseLength(size_t index) const {
  switch (kind) {
    case Kind::Data: {
      if (index < pilotPulses) return pilotLength;
      if (index == pilotPulses) return syncFirst;
      if (index == pilotPulses + 1) return syncSecond;
      if (index + 1 >= pulseCount()) return tailLength;
      const size_t bit = (index - pilotPulses - 2) / 2;
      if (bit / 8 >= data.size()) return tailLength;
      const u8 value = data[bit / 8];
      return ((value >> (7 - (bit % 8))) & 1) ? oneLength : zeroLength;
    }
    case Kind::Pulses:
      return pulseLengths.empty() ? 1 : pulseLengths[index % pulseLengths.size()];
    case Kind::Direct: return sampleTstates;
    case Kind::Pause: return pauseMs * TstatesPerMs;
  }
  return 1;
}

bool parseTap(const std::vector<u8>& bytes, TapeImage& out, std::string& error) {
  out = TapeImage{};
  out.title = "TAP";

  size_t pos = 0;
  while (pos + 2 <= bytes.size()) {
    const u16 length = u16(bytes[pos] | (u16(bytes[pos + 1]) << 8));
    pos += 2;
    if (length == 0) continue; // some tools leave zero-length padding behind
    if (pos + length > bytes.size()) {
      error = "TAP block runs past the end of the file";
      return false;
    }
    out.blocks.push_back(standardData(std::vector<u8>(bytes.begin() + long(pos),
                                                     bytes.begin() + long(pos + length))));
    pos += length;
    if (out.blocks.size() > MaxBlocks) {
      error = "TAP file has too many blocks";
      return false;
    }
  }

  if (out.blocks.empty()) {
    error = "TAP file contains no blocks";
    return false;
  }
  return true;
}

bool parseTzx(const std::vector<u8>& bytes, TapeImage& out, std::string& error) {
  if (bytes.size() < 10 || !std::equal(TzxMagic, TzxMagic + 8, bytes.begin())) {
    error = "not a TZX file";
    return false;
  }

  out = TapeImage{};
  out.fromTzx = true;
  out.title = "TZX";

  Reader reader(bytes);
  reader.seek(10); // magic, version major, version minor

  struct LoopPoint {
    size_t resume; // first byte after the loop-start block
    u32 remaining;
  };
  std::vector<LoopPoint> loops;

  while (reader.pos() < reader.size()) {
    if (out.blocks.size() > MaxBlocks) {
      error = "TZX file expands to too many blocks (runaway loop)";
      return false;
    }

    const size_t blockStart = reader.pos();
    const u8 id = reader.byte();

    switch (id) {
    case 0x10: { // standard speed data
      if (!reader.need(4)) return truncated(error);
      const u16 pause = reader.word();
      const u16 length = reader.word();
      if (!reader.need(length)) return truncated(error);
      TapeBlock block = standardData(reader.bytes(length));
      block.pauseMs = pause;
      out.blocks.push_back(std::move(block));
      break;
    }
    case 0x11: { // turbo speed data
      if (!reader.need(18)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Data;
      block.pilotLength = reader.word();
      block.syncFirst = reader.word();
      block.syncSecond = reader.word();
      block.zeroLength = reader.word();
      block.oneLength = reader.word();
      block.pilotPulses = reader.word();
      block.usedBitsLastByte = reader.byte();
      block.pauseMs = reader.word();
      const u32 length = reader.triple();
      if (!reader.need(length)) return truncated(error);
      block.data = reader.bytes(length);
      out.blocks.push_back(std::move(block));
      break;
    }
    case 0x12: { // pure tone
      if (!reader.need(4)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Pulses;
      block.pulseLengths.push_back(reader.word());
      block.pulseRepeat = reader.word();
      if (block.pulseRepeat > 0) out.blocks.push_back(std::move(block));
      break;
    }
    case 0x13: { // pulse sequence
      if (!reader.need(1)) return truncated(error);
      const u8 count = reader.byte();
      if (!reader.need(size_t(count) * 2)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Pulses;
      for (u8 i = 0; i < count; ++i) block.pulseLengths.push_back(reader.word());
      if (!block.pulseLengths.empty()) out.blocks.push_back(std::move(block));
      break;
    }
    case 0x14: { // pure data
      if (!reader.need(10)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Data;
      block.zeroLength = reader.word();
      block.oneLength = reader.word();
      block.usedBitsLastByte = reader.byte();
      block.pauseMs = reader.word();
      const u32 length = reader.triple();
      if (!reader.need(length)) return truncated(error);
      block.data = reader.bytes(length);
      out.blocks.push_back(std::move(block));
      break;
    }
    case 0x15: { // direct recording
      if (!reader.need(8)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Direct;
      block.sampleTstates = reader.word();
      block.pauseMs = reader.word();
      block.usedBitsLastByte = reader.byte();
      const u32 length = reader.triple();
      if (!reader.need(length)) return truncated(error);
      block.samples = reader.bytes(length);
      const u32 used = block.usedBitsLastByte == 0 ? 8 : std::min<u32>(block.usedBitsLastByte, 8);
      block.sampleBits = u32(block.samples.size()) * 8 - (8 - used);
      if (block.sampleBits > 0) out.blocks.push_back(std::move(block));
      break;
    }
    case 0x18:   // CSW recording
    case 0x19:   // generalised data
    case 0x28:   // select block
    case 0x2A:   // stop if 48K
    case 0x32: { // archive info
      if (!reader.need(4)) return truncated(error);
      const u32 length = reader.quad();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x20: { // pause
      if (!reader.need(2)) return truncated(error);
      TapeBlock block;
      block.kind = TapeBlock::Kind::Pause;
      block.pauseMs = reader.word();
      if (block.pauseMs > 0) out.blocks.push_back(std::move(block));
      break;
    }
    case 0x21: { // group start
      if (!reader.need(1)) return truncated(error);
      const u8 length = reader.byte();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x22: // group end
      break;
    case 0x25: // loop end
      if (!loops.empty()) {
        LoopPoint& loop = loops.back();
        if (--loop.remaining > 0) {
          reader.seek(loop.resume);
        } else {
          loops.pop_back();
        }
      }
      break;
    case 0x23: { // jump to relative position
      if (!reader.need(2)) return truncated(error);
      const i32 offset = i32(i16(reader.word()));
      const long long target = static_cast<long long>(blockStart) + 1 + offset;
      if (target < 10 || target >= static_cast<long long>(reader.size())) {
        error = "TZX jump leaves the file";
        return false;
      }
      reader.seek(size_t(target));
      break;
    }
    case 0x24: { // loop start
      if (!reader.need(2)) return truncated(error);
      const u16 count = reader.word();
      loops.push_back(LoopPoint{reader.pos(), count == 0 ? 1u : u32(count)});
      break;
    }
    case 0x26: { // call sequence
      if (!reader.need(2)) return truncated(error);
      const u16 length = reader.word();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x27: // return from call sequence
      break;
    case 0x2B: { // set signal level
      if (!reader.need(5)) return truncated(error);
      const u32 length = reader.quad();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x30: { // text description
      if (!reader.need(1)) return truncated(error);
      const u8 length = reader.byte();
      if (!reader.need(length)) return truncated(error);
      const std::string text = reader.text(length);
      if (out.title == "TZX" && !text.empty()) out.title = text;
      break;
    }
    case 0x31: { // message block
      if (!reader.need(2)) return truncated(error);
      reader.byte(); // display duration
      const u8 length = reader.byte();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x33: { // hardware type
      if (!reader.need(1)) return truncated(error);
      const u8 count = reader.byte();
      if (!reader.need(size_t(count) * 3)) return truncated(error);
      reader.skip(size_t(count) * 3);
      break;
    }
    case 0x35: { // custom info
      if (!reader.need(20)) return truncated(error);
      reader.skip(16);
      const u32 length = reader.quad();
      if (!reader.need(length)) return truncated(error);
      reader.skip(length);
      break;
    }
    case 0x5A: // glue block
      if (!reader.need(9)) return truncated(error);
      reader.skip(9);
      break;
    default: {
      char message[64];
      std::snprintf(message, sizeof message, "unsupported TZX block 0x%02X", unsigned(id));
      error = message;
      return false;
    }
    }
  }

  if (out.blocks.empty()) {
    error = "TZX file contains no playable blocks";
    return false;
  }
  return true;
}

bool loadTapeFile(const std::string& path, TapeImage& out, std::string& error) {
  std::vector<u8> bytes;
  if (!readFile(path, bytes, error)) return false;

  if (bytes.size() >= 8 && std::equal(TzxMagic, TzxMagic + 8, bytes.begin())) {
    return parseTzx(bytes, out, error);
  }
  return parseTap(bytes, out, error);
}

} // namespace rainbow::formats
