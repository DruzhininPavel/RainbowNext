#include "core/tape.h"

#include <algorithm>
#include <cstdio>

namespace rainbow {

bool Tape::load(const std::string& path, std::string& error) {
  formats::TapeImage image;
  if (!formats::loadTapeFile(path, image, error)) return false;
  image_ = std::move(image);
  rewind();
  return true;
}

void Tape::loadImage(formats::TapeImage image) {
  image_ = std::move(image);
  rewind();
}

void Tape::eject() {
  image_ = formats::TapeImage{};
  rewind();
}

void Tape::rewind() {
  block_ = 0;
  pulse_ = 0;
  remaining_ = 0;
  ear_ = true;
  playing_ = false;
}

void Tape::play() {
  if (!loaded()) return;
  if (atEnd()) rewind();
  playing_ = true;
}

void Tape::stop() { playing_ = false; }

void Tape::setPlaying(bool playing) {
  if (playing) {
    play();
  } else {
    stop();
  }
}

bool Tape::atEnd() const { return !loaded() || block_ >= image_.blocks.size(); }

std::string Tape::currentBlockName() const {
  if (!loaded()) return "no tape";
  if (block_ >= image_.blocks.size()) return "end of tape";
  const formats::TapeBlock& block = image_.blocks[block_];
  char buffer[64];
  switch (block.kind) {
    case formats::TapeBlock::Kind::Data:
      std::snprintf(buffer, sizeof buffer, "%s %zu/%zu",
                    block.isHeader() ? "header" : "data", block_ + 1, image_.blocks.size());
      break;
    case formats::TapeBlock::Kind::Pause:
      std::snprintf(buffer, sizeof buffer, "pause %zu/%zu", block_ + 1, image_.blocks.size());
      break;
    default:
      std::snprintf(buffer, sizeof buffer, "tone %zu/%zu", block_ + 1, image_.blocks.size());
      break;
  }
  return buffer;
}

bool Tape::nextPulse() {
  while (block_ < image_.blocks.size()) {
    const formats::TapeBlock& block = image_.blocks[block_];
    if (pulse_ >= block.pulseCount()) {
      ++block_;
      pulse_ = 0;
      continue;
    }

    remaining_ = std::max<u32>(1, block.pulseLength(pulse_));
    if (block.kind == formats::TapeBlock::Kind::Direct) {
      const size_t bit = pulse_;
      const u8 value = bit / 8 < block.samples.size() ? block.samples[bit / 8] : 0;
      ear_ = ((value >> (7 - (bit % 8))) & 1) != 0;
    } else {
      // Every pulse is a half period, so the level flips at each one.
      ear_ = !ear_;
    }
    ++pulse_;
    return true;
  }
  return false;
}

void Tape::advance(u32 tstates) {
  if (!playing_) return;
  while (tstates > 0) {
    if (remaining_ == 0 && !nextPulse()) {
      playing_ = false;
      return;
    }
    const u32 step = std::min(tstates, remaining_);
    remaining_ -= step;
    tstates -= step;
  }
  // Land on the boundary that starts now, so a port read at this instant sees
  // the new level instead of the pulse that has just ended.
  if (remaining_ == 0 && !nextPulse()) playing_ = false;
}

const formats::TapeBlock* Tape::seekDataBlock() {
  while (block_ < image_.blocks.size() &&
         image_.blocks[block_].kind != formats::TapeBlock::Kind::Data) {
    ++block_;
    pulse_ = 0;
  }
  if (block_ >= image_.blocks.size()) return nullptr;
  return &image_.blocks[block_];
}

void Tape::stepBlock() {
  if (block_ < image_.blocks.size()) ++block_;
  pulse_ = 0;
  remaining_ = 0;
}

} // namespace rainbow
