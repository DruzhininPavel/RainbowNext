#include "frontend/autoload.h"

#include "core/tape.h"

namespace rainbow::frontend {

namespace {

// How long a key is held and released for. The ROM scans the matrix once per
// frame and the 128K menu also runs a debounce loop, so a single frame is not
// enough to be seen as a press.
constexpr int kHoldFrames = 4;

// Frames to wait before the first key, so the ROM has reached the prompt or the
// menu. The 48K machines print their banner in about a second; the 128K ones
// show the menu almost as fast, but it is a slower ROM overall, and NextZXOS
// takes the longest of all.
int bootFrames(MachineType type) {
  switch (type) {
    case MachineType::Spectrum48:
    case MachineType::Pentagon: return 130;
    case MachineType::Spectrum128:
    case MachineType::Plus2:
    case MachineType::Plus3: return 200;
    case MachineType::Next: return 400;
  }
  return 200;
}

// Keyboard matrix positions. Rows follow the standard Spectrum layout, so these
// are the same on every machine the emulator supports.
constexpr int kRowEnter = 6;
constexpr int kColumnEnter = 0;
constexpr int kRowJ = 6;
constexpr int kColumnJ = 3;
constexpr int kRowP = 5;
constexpr int kColumnP = 0;
constexpr int kRowSymbolShift = 7;
constexpr int kColumnSymbolShift = 1;

} // namespace

void Autoload::wait(int frames) {
  Step step;
  step.frames = frames;
  steps_.push_back(step);
}

void Autoload::press(int row, int column) {
  Step down;
  down.row = row;
  down.column = column;
  down.frames = kHoldFrames;
  down.down = true;
  steps_.push_back(down);

  Step up = down;
  up.down = false;
  steps_.push_back(up);
}

void Autoload::pressSymbolShift(int row, int column) {
  // SYMBOL SHIFT is a shift, so it has to be held across the other key press
  // instead of being tapped on its own.
  Step down;
  down.row = kRowSymbolShift;
  down.column = kColumnSymbolShift;
  down.frames = kHoldFrames;
  down.down = true;
  steps_.push_back(down);

  press(row, column);

  Step up = down;
  up.down = false;
  steps_.push_back(up);
}

void Autoload::start(Machine& machine) {
  cancel();
  machine_ = &machine;
  wait(bootFrames(machine.type()));
  if (machine.type() == MachineType::Spectrum128 || machine.type() == MachineType::Plus2 ||
      machine.type() == MachineType::Plus3) {
    // ENTER on the boot menu picks "Tape Loader", which issues LOAD "" itself.
    press(kRowEnter, kColumnEnter);
    return;
  }
  // J is the LOAD keyword, and SHIFT+P types the quote that asks the ROM for
  // the next file on the tape.
  press(kRowJ, kColumnJ);
  pressSymbolShift(kRowP, kColumnP);
  pressSymbolShift(kRowP, kColumnP);
  press(kRowEnter, kColumnEnter);
}

void Autoload::cancel() {
  if (machine_ != nullptr && next_ > 0 && next_ <= steps_.size()) {
    // A sequence can be dropped halfway through a press, and a key left down
    // would keep the ROM from ever reading the tape.
    const Step& last = steps_[next_ - 1];
    if (last.down) machine_->keyboard().setKey(last.row, last.column, false);
  }
  machine_ = nullptr;
  steps_.clear();
  next_ = 0;
  remaining_ = 0;
}

void Autoload::apply(const Step& step) {
  if (machine_ == nullptr) return;
  machine_->keyboard().setKey(step.row, step.column, step.down);
}

void Autoload::tick() {
  if (!active() || machine_ == nullptr) return;
  if (remaining_ > 0) {
    --remaining_;
    return;
  }
  const Step& step = steps_[next_++];
  apply(step);
  remaining_ = step.frames - 1;
  if (!active()) {
    // The ROM is now waiting for the tape, so the deck can start.
    machine_->tape().play();
  }
}

} // namespace rainbow::frontend
