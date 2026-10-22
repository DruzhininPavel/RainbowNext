#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "core/types.h"

namespace rainbow {

// A ring of save states, newest last, that turns the emulator into a machine
// with an undo key.
//
// The buffer is bounded by bytes rather than by a count, because the states
// differ hugely in size: a 48K Spectrum costs a few hundred kilobytes and a
// Next with 2 MB of RAM a few megabytes. When the next state would not fit, the
// oldest one is dropped, so rewinding always reaches back as far as the memory
// allows instead of being pinned to a fixed number of steps.
class RewindBuffer {
public:
  // 256 MB is roughly a minute of 48K play and half a minute of Next play.
  static constexpr size_t DefaultBytes = 256u << 20;

  explicit RewindBuffer(size_t maxBytes = DefaultBytes) : maxBytes_(maxBytes) {}

  // Adds a state as the newest one. A state too big to ever fit is dropped
  // rather than emptying the buffer around it.
  void push(std::vector<u8> state);

  bool canRewind() const { return !states_.empty(); }
  // Hands back the newest state and forgets it, so repeated calls walk back
  // through the recording.
  bool pop(std::vector<u8>& out);

  void clear();

  size_t count() const { return states_.size(); }
  size_t bytes() const { return bytes_; }
  size_t maxBytes() const { return maxBytes_; }

private:
  std::deque<std::vector<u8>> states_;
  size_t maxBytes_ = DefaultBytes;
  size_t bytes_ = 0;
};

} // namespace rainbow
