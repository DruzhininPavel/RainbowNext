#include "core/rzx_session.h"

#include <utility>

namespace rainbow {

namespace {

// The program name an RZX creator block carries, and the version of the
// emulator that wrote it.
constexpr char kProgram[] = "RainbowNext";
constexpr u16 kProgramMajor = 0;
constexpr u16 kProgramMinor = 1;

// A frame stores its instruction count in 16 bits; a frame of the slowest
// machine runs far fewer than that, so a clamp only ever matters if something
// upstream has already gone wrong.
u16 clampInstructions(u64 count) {
  return count < 0xFFFF ? u16(count) : 0xFFFF;
}

} // namespace

RzxSession::~RzxSession() { unbind(); }

void RzxSession::bind(Machine& machine) {
  if (machine_ != nullptr && machine_ != &machine) unbind();
  machine_ = &machine;
  machine_->setIoReadHook(this);
}

void RzxSession::unbind() {
  if (machine_ != nullptr) machine_->setIoReadHook(nullptr);
  machine_ = nullptr;
  resetState();
}

void RzxSession::resetState() {
  mode_ = Mode::Off;
  file_ = rzx::File{};
  frameInput_.clear();
  instructionsAtFrameStart_ = 0;
  recorded_ = 0;
  frames_.clear();
  frameIndex_ = 0;
  inputIndex_ = 0;
  played_ = 0;
  desyncs_ = 0;
}

bool RzxSession::startRecording(Machine& machine, std::string& error) {
  if (mode_ != Mode::Off) {
    error = "a recording or a replay is already running";
    return false;
  }
  if (machine_ != &machine) bind(machine);

  // The snapshot is what makes the input stream meaningful: it is the machine
  // the first frame was captured on. Our own state format is used because the
  // RZX snapshot slot takes any four-character name, and this one round-trips
  // every machine RainbowNext emulates.
  std::vector<u8> state;
  machine.saveState(state);
  if (state.empty()) {
    error = "cannot snapshot the machine";
    return false;
  }

  resetState();
  file_.creator = rzx::Creator{kProgram, kProgramMajor, kProgramMinor};
  rzx::Block snapshot;
  snapshot.kind = rzx::Block::Kind::Snapshot;
  snapshot.snapshot.format = "RBNS";
  snapshot.snapshot.data = std::move(state);
  file_.blocks.push_back(std::move(snapshot));
  // The clock the block starts at is the machine's, which counts frames, so it
  // cannot drift from the input it describes.
  file_.appendInput(u32(machine.frames() * machine.tstatesPerFrameValue()));

  instructionsAtFrameStart_ = machine.instructions();
  mode_ = Mode::Record;
  return true;
}

bool RzxSession::takeRecording(rzx::File& out) {
  if (mode_ != Mode::Record) return false;
  out = std::move(file_);
  resetState();
  return true;
}

void RzxSession::cancel() {
  if (mode_ == Mode::Off) return;
  resetState();
}

void RzxSession::startPlayback(rzx::File file) {
  resetState();
  file_ = std::move(file);
  mode_ = Mode::Play;

  // Frames are taken from the first run of input blocks. A snapshot further in
  // would need the machine rebuilt mid-replay, so the run ends there and the
  // frontend reports it rather than continuing on the wrong state.
  for (const rzx::Block& block : file_.blocks) {
    if (block.kind == rzx::Block::Kind::Snapshot) {
      if (!frames_.empty()) break;
      continue;
    }
    for (const rzx::Frame& frame : block.input.frames) {
      // A repeat frame carries no bytes; it names the frame before it.
      if (frame.repeat) {
        if (!frames_.empty()) frames_.push_back(frames_.back());
        continue;
      }
      frames_.push_back(&frame);
    }
  }
}

bool RzxSession::frameFinished(const Machine& machine) {
  if (mode_ == Mode::Record) {
    rzx::InputBlock& block = file_.appendInput(u32(machine.frames() * machine.tstatesPerFrameValue()));
    rzx::Frame frame;
    frame.instructions = clampInstructions(machine.instructions() - instructionsAtFrameStart_);
    frame.input = std::move(frameInput_);
    frameInput_.clear();
    block.frames.push_back(std::move(frame));
    instructionsAtFrameStart_ = machine.instructions();
    ++recorded_;
    return true;
  }

  if (mode_ != Mode::Play) return false;

  // Bytes the machine never asked for mean the replay has drifted, but there is
  // nothing better to do than note it and move on: the recording is what it is.
  if (frameIndex_ < frames_.size() && inputIndex_ < frames_[frameIndex_]->input.size()) {
    ++desyncs_;
  }
  ++played_;
  ++frameIndex_;
  inputIndex_ = 0;
  if (frameIndex_ >= frames_.size()) {
    mode_ = Mode::Off;
    return false;
  }
  return true;
}

void RzxSession::frameAborted() {
  if (mode_ != Mode::Record) return;
  frameInput_.clear();
  if (machine_ != nullptr) instructionsAtFrameStart_ = machine_->instructions();
}

u8 RzxSession::ioRead(u16, u8 value) {
  switch (mode_) {
    case Mode::Record:
      if (frameInput_.size() < 0xFFFE) frameInput_.push_back(value);
      return value;

    case Mode::Play: {
      if (frameIndex_ >= frames_.size()) {
        ++desyncs_;
        return value;
      }
      const std::vector<u8>& input = frames_[frameIndex_]->input;
      if (inputIndex_ >= input.size()) {
        // The recording has no byte left for this read. 0xFF is what an
        // unclaimed port returns, which keeps the run going instead of feeding
        // the program whatever the keyboard happens to say.
        ++desyncs_;
        return 0xFF;
      }
      return input[inputIndex_++];
    }

    case Mode::Off:
      break;
  }
  return value;
}

} // namespace rainbow
