#pragma once

#include <string>
#include <vector>

#include "core/types.h"
#include "formats/tape.h"

namespace mynext {

// The virtual cassette: a list of pulse blocks played back against the
// machine's T-state clock, plus the shortcut that serves the ROM's LD-BYTES
// routine straight from the file.
//
// Playback is pulse level, not byte level, so loaders that ignore the ROM
// (turbo loaders, speedloaders, protection schemes) work exactly as they do on
// real hardware: the ULA just sees the EAR bit toggling.
class Tape {
public:
  bool load(const std::string& path, std::string& error);
  void loadImage(formats::TapeImage image);
  void eject();

  bool loaded() const { return !image_.blocks.empty(); }
  const std::string& title() const { return image_.title; }
  size_t blockCount() const { return image_.blocks.size(); }
  size_t blockIndex() const { return block_; }

  // A human readable name for the block under the head, used by the status bar.
  std::string currentBlockName() const;

  void rewind();
  void play();
  void stop();
  void setPlaying(bool playing);
  bool playing() const { return playing_; }
  bool atEnd() const;

  void setFastLoad(bool on) { fastLoad_ = on; }
  bool fastLoad() const { return fastLoad_; }

  // Runs the tape for `tstates` of machine time.
  void advance(u32 tstates);

  // EAR bit as seen by port 0xFE. Idle tape reads high.
  bool ear() const { return ear_; }

  // --- fast load ----------------------------------------------------------
  // Skips tone and pause blocks and returns the data block the head now rests
  // on, or nullptr at the end of the tape.
  const formats::TapeBlock* seekDataBlock();
  // Moves the head past the block it currently rests on.
  void stepBlock();

private:
  bool nextPulse();

  formats::TapeImage image_;
  size_t block_ = 0;
  size_t pulse_ = 0;
  u32 remaining_ = 0;
  bool ear_ = true;
  bool playing_ = false;
  bool fastLoad_ = true;
};

} // namespace mynext
