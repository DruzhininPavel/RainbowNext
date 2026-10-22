#pragma once

#include <cstddef>
#include <vector>

#include "core/machine.h"

namespace rainbow::frontend {

// Types the key sequence a stock ROM needs before it starts reading the tape.
// The 48K machines need `LOAD ""` at the BASIC prompt; on the 128K ones the
// first entry of the boot menu is already the tape loader, so ENTER is enough.
// The sequence is a list of timed key presses, because the ROM only samples the
// matrix once per frame and needs the key held across a scan.
class Autoload {
public:
  // Arms the sequence for a machine that already has a tape in the deck. The
  // machine is borrowed, not owned, so the caller must cancel() before it
  // replaces or destroys the machine.
  void start(Machine& machine);
  // Drops whatever is left of the sequence and lets go of the key it is holding.
  void cancel();
  bool active() const { return next_ < steps_.size(); }

  // Consumes one frame of the sequence. Call it once per frame after the
  // machine has run, so the ROM has already scanned the matrix.
  void tick();

private:
  struct Step {
    int row = 0;
    int column = 0;
    int frames = 0;
    bool down = false;
  };

  void wait(int frames);
  void press(int row, int column);
  void pressSymbolShift(int row, int column);
  void apply(const Step& step);

  Machine* machine_ = nullptr;
  std::vector<Step> steps_;
  size_t next_ = 0;
  int remaining_ = 0;
};

} // namespace rainbow::frontend
