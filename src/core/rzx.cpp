#include "core/rzx.h"

#include <cstring>

namespace rainbow {
namespace rzx {

namespace {

constexpr char kSignature[4] = {'R', 'Z', 'X', '!'};
// Every reader in the wild understands 0.12, the last version that was not
// about signed recordings, so that is what we claim to be.
constexpr u8 kMajorVersion = 0;
constexpr u8 kMinorVersion = 12;
// The block header itself is five bytes: one identifier, four of length.
constexpr size_t kBlockHeader = 5;
// The count field of a frame uses this instead of a length to mean "play the
// frame before this one again".
constexpr u16 kRepeatFrame = 0xFFFF;

u16 readWord(const u8* p) { return u16(p[0] | (u16(p[1]) << 8)); }

u32 readDword(const u8* p) {
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

void pushWord(std::vector<u8>& out, u16 value) {
  out.push_back(u8(value & 0xFF));
  out.push_back(u8(value >> 8));
}

void pushDword(std::vector<u8>& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(u8((value >> (8 * i)) & 0xFF));
}

void pushBlock(std::vector<u8>& out, u8 id, const std::vector<u8>& body) {
  out.push_back(id);
  pushDword(out, u32(body.size() + kBlockHeader));
  out.insert(out.end(), body.begin(), body.end());
}

bool readCreator(const u8* body, size_t size, File& out, std::string& error) {
  if (size < 24) {
    error = "creator block is too short";
    return false;
  }
  size_t length = 0;
  while (length < 20 && body[length] != 0) ++length;
  out.creator.program.assign(reinterpret_cast<const char*>(body), length);
  out.creator.major = readWord(body + 20);
  out.creator.minor = readWord(body + 22);
  return true;
}

bool readSnapshot(const u8* body, size_t size, File& out, std::string& error) {
  if (size < 12) {
    error = "snapshot block is too short";
    return false;
  }
  const u32 flags = readDword(body);
  // The snapshot lives in another file. Nothing to read, and nothing to replay
  // from, so the block is skipped the way libspectrum skips it.
  if ((flags & 0x01) != 0) return true;
  if ((flags & 0x02) != 0) {
    error = "compressed snapshots are not supported";
    return false;
  }

  Block block;
  block.kind = Block::Kind::Snapshot;
  block.snapshot.format.assign(reinterpret_cast<const char*>(body + 4), 4);
  while (!block.snapshot.format.empty() && block.snapshot.format.back() == '\0') {
    block.snapshot.format.pop_back();
  }
  const u32 length = readDword(body + 8);
  if (size - 12 < length) {
    error = "snapshot block is truncated";
    return false;
  }
  block.snapshot.data.assign(body + 12, body + 12 + length);
  out.blocks.push_back(std::move(block));
  return true;
}

bool readInput(const u8* body, size_t size, File& out, std::string& error) {
  if (size < 13) {
    error = "input block is too short";
    return false;
  }
  const u32 count = readDword(body);
  // One byte sits between the frame count and the clock: each frame's length is
  // undefined, and every writer stores a zero there.
  const u32 tstates = readDword(body + 5);
  const u32 flags = readDword(body + 9);
  if ((flags & 0x02) != 0) {
    error = "compressed input is not supported";
    return false;
  }

  Block block;
  block.kind = Block::Kind::Input;
  block.input.tstates = tstates;
  block.input.frames.reserve(count);

  const u8* ptr = body + 13;
  const u8* end = body + size;
  for (u32 i = 0; i < count; ++i) {
    if (end - ptr < 4) {
      error = "input block is truncated";
      return false;
    }
    Frame frame;
    frame.instructions = readWord(ptr);
    const u16 length = readWord(ptr + 2);
    ptr += 4;
    if (length == kRepeatFrame) {
      frame.repeat = true;
    } else {
      if (size_t(end - ptr) < length) {
        error = "input block is truncated";
        return false;
      }
      frame.input.assign(ptr, ptr + length);
      ptr += length;
    }
    block.input.frames.push_back(std::move(frame));
  }
  out.blocks.push_back(std::move(block));
  return true;
}

} // namespace

InputBlock& File::appendInput(u32 tstates) {
  if (!blocks.empty() && blocks.back().kind == Block::Kind::Input) {
    return blocks.back().input;
  }
  Block block;
  block.kind = Block::Kind::Input;
  block.input.tstates = tstates;
  blocks.push_back(std::move(block));
  return blocks.back().input;
}

bool parse(const std::vector<u8>& data, File& out, std::string& error) {
  out = File{};
  if (data.size() < 10 || std::memcmp(data.data(), kSignature, 4) != 0) {
    error = "not an RZX file";
    return false;
  }
  // The version numbers and the flags describe signing, which is not something
  // this build reads, so the header is consumed and forgotten.
  size_t pos = 10;
  while (pos < data.size()) {
    if (data.size() - pos < kBlockHeader) {
      error = "truncated block header";
      return false;
    }
    const u8 id = data[pos];
    const u32 length = readDword(&data[pos + 1]);
    if (length < kBlockHeader) {
      error = "block length " + std::to_string(length) + " is too small";
      return false;
    }
    if (length > data.size() - pos) {
      error = "block at offset " + std::to_string(pos) + " runs past the end of the file";
      return false;
    }
    const u8* body = &data[pos + kBlockHeader];
    const size_t bodySize = length - kBlockHeader;

    bool ok = true;
    switch (id) {
      case BlockCreator: ok = readCreator(body, bodySize, out, error); break;
      case BlockSnapshot: ok = readSnapshot(body, bodySize, out, error); break;
      case BlockInput: ok = readInput(body, bodySize, out, error); break;
      default: break;  // signatures, and anything a later version adds
    }
    if (!ok) return false;
    pos += length;
  }
  return true;
}

void write(const File& file, std::vector<u8>& out) {
  out.clear();
  out.insert(out.end(), kSignature, kSignature + 4);
  out.push_back(kMajorVersion);
  out.push_back(kMinorVersion);
  pushDword(out, 0);  // flags: not signed, blocks not compressed

  std::vector<u8> body(file.creator.program.begin(), file.creator.program.end());
  body.resize(20, 0);
  pushWord(body, file.creator.major);
  pushWord(body, file.creator.minor);
  pushBlock(out, BlockCreator, body);

  for (const Block& block : file.blocks) {
    body.clear();
    if (block.kind == Block::Kind::Snapshot) {
      pushDword(body, 0);  // flags: neither a link nor compressed
      std::string format = block.snapshot.format.substr(0, 4);
      // libspectrum compares all four bytes against "SNA\0", so the padding has
      // to be a zero and not a space.
      format.resize(4, '\0');
      body.insert(body.end(), format.begin(), format.end());
      pushDword(body, u32(block.snapshot.data.size()));
      body.insert(body.end(), block.snapshot.data.begin(), block.snapshot.data.end());
      pushBlock(out, BlockSnapshot, body);
      continue;
    }

    pushDword(body, u32(block.input.frames.size()));
    body.push_back(0);  // each frame's length is undefined
    pushDword(body, block.input.tstates);
    pushDword(body, 0);  // flags: not compressed
    for (const Frame& frame : block.input.frames) {
      pushWord(body, frame.instructions);
      if (frame.repeat) {
        pushWord(body, kRepeatFrame);
        continue;
      }
      // A frame holds a 16-bit count, and 0xFFFF is taken by the repeat marker,
      // so the last value a real length can have is 0xFFFE.
      const size_t length = frame.input.size() < kRepeatFrame ? frame.input.size() : kRepeatFrame - 1;
      pushWord(body, u16(length));
      body.insert(body.end(), frame.input.begin(), frame.input.begin() + length);
    }
    pushBlock(out, BlockInput, body);
  }
}

} // namespace rzx
} // namespace rainbow
