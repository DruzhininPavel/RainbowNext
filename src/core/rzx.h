#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow {
namespace rzx {

// The container RZX. A file is a ten-byte header followed by blocks, each
// introduced by a one-byte identifier and a 32-bit length that counts its own
// five header bytes. The layout here is what libspectrum and Fuse read and
// write, which is the only description of the format that exists in practice.
enum BlockId : u8 {
  BlockCreator = 0x10,
  BlockSignStart = 0x20,
  BlockSignEnd = 0x21,
  BlockSnapshot = 0x30,
  BlockInput = 0x80,
};

// What the recording was made with. RZX calls it the creator block.
struct Creator {
  std::string program;  // truncated to 20 bytes on the way out
  u16 major = 0;
  u16 minor = 0;
};

// One emulated frame of input. `input` holds the byte every port read returned,
// in the order the CPU asked for them -- not a picture of the keyboard, which is
// what makes a recording reproduce a machine exactly.
struct Frame {
  u16 instructions = 0;
  // The count field was 0xFFFF, so this frame replays the one before it and
  // carries no bytes of its own.
  bool repeat = false;
  std::vector<u8> input;
};

// A run of frames. libspectrum writes one per recording, and so do we; more
// than one only appears in files that were spliced together.
struct InputBlock {
  u32 tstates = 0;  // machine clock at the start of the block
  std::vector<Frame> frames;
};

// A snapshot carried inside the file, in one of the formats libspectrum knows
// by a four-character name: "SNA", "SZX", "Z80" -- or "RBNS" for our own.
struct SnapshotBlock {
  std::string format;
  std::vector<u8> data;
};

struct Block {
  enum class Kind { Snapshot, Input };
  Kind kind = Kind::Input;
  SnapshotBlock snapshot;
  InputBlock input;
};

struct File {
  Creator creator;
  std::vector<Block> blocks;

  // The block new frames are appended to, starting one if there is none. Kept
  // last so a recording always reads snapshot-then-input.
  InputBlock& appendInput(u32 tstates);
};

// Reads a whole file. Signature blocks and identifiers this build does not know
// are skipped; a truncated block or a contradictory length is an error.
bool parse(const std::vector<u8>& data, File& out, std::string& error);

// Writes the file with uncompressed blocks. Compression is optional in the
// format -- the flag simply stays clear -- and that keeps zlib out of the
// dependency list.
void write(const File& file, std::vector<u8>& out);

} // namespace rzx
} // namespace rainbow
