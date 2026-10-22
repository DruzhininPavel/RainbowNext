#pragma once

#include "core/types.h"

namespace rainbow {

// The 1-bit beeper driven by bit 4 of port 0xFE. The machine samples it at the
// audio rate while ticking, which is accurate enough for M0 and avoids the
// aliasing of frame-rate sampling.
class Beeper {
public:
  void reset() { level_ = 0.0f; }
  void setLevel(u8 level) { level_ = level ? 0.5f : -0.5f; }
  float sample() const { return level_; }

private:
  float level_ = 0.0f;
};

} // namespace rainbow
