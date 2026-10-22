#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow::formats {

// A tape block reduced to what the emulator actually needs: a pulse pattern
// (tone, pause, direct recording) or a data block described by the pilot,
// sync and bit pulse lengths of the loader it was recorded with.
//
// TAP and TZX both normalise into this list so the tape engine only ever deals
// with one representation, and a block's pulse train can be regenerated
// exactly from a handful of numbers instead of being stored as samples.
struct TapeBlock {
  enum class Kind {
    Data,   // pilot tone + sync + data bits + tail, with a trailing pause
    Pulses, // a repeating pattern of pulse lengths (TZX pure tone / pulses)
    Direct, // one bit per sample at a fixed T-state interval
    Pause,  // silence on the tape
  };

  Kind kind = Kind::Data;
  std::string name; // optional label from a TZX text or group block

  // Kind::Data. The payload starts with the flag byte (0x00 for a header,
  // 0xFF for a data block) exactly as the ROM reads it back.
  std::vector<u8> data;
  u32 pilotPulses = 0;
  u32 pilotLength = 0;
  u32 syncFirst = 0;
  u32 syncSecond = 0;
  u32 zeroLength = 0;
  u32 oneLength = 0;
  u32 tailLength = 0;
  u32 usedBitsLastByte = 8;

  // Kind::Pulses.
  std::vector<u32> pulseLengths;
  u32 pulseRepeat = 1;

  // Kind::Direct.
  std::vector<u8> samples;
  u32 sampleBits = 0;
  u32 sampleTstates = 79;

  // Kind::Pause, and the pause that follows a data block.
  u32 pauseMs = 0;

  size_t bitCount() const;
  u64 pulseCount() const;
  u32 pulseLength(size_t index) const;
  bool isHeader() const { return kind == Kind::Data && !data.empty() && data[0] == 0x00; }
};

struct TapeImage {
  std::string title;
  std::vector<TapeBlock> blocks;
  bool fromTzx = false;
};

// The pulse lengths the ROM's own loader produces, used for TAP files and for
// TZX blocks that ask for "standard speed".
inline constexpr u32 StandardPilotPulses = 8063;
inline constexpr u32 StandardPilotLength = 2168;
inline constexpr u32 StandardSyncFirst = 667;
inline constexpr u32 StandardSyncSecond = 735;
inline constexpr u32 StandardZeroLength = 855;
inline constexpr u32 StandardOneLength = 1710;
inline constexpr u32 StandardTailLength = 1000;
inline constexpr u32 StandardPauseMs = 1000;

bool parseTap(const std::vector<u8>& bytes, TapeImage& out, std::string& error);
bool parseTzx(const std::vector<u8>& bytes, TapeImage& out, std::string& error);

// Sniffs the container and dispatches: TZX by its magic, TAP otherwise.
bool loadTapeFile(const std::string& path, TapeImage& out, std::string& error);

} // namespace rainbow::formats
