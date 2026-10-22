#pragma once

#include <string>
#include <vector>

#include "core/machine.h"
#include "core/rzx.h"

namespace rainbow {

// Records and replays RZX. What the format stores is the byte every port read
// returned, not the state of the keyboard, so a session has to sit on the
// machine's I/O path rather than on the input layer: that is the only place
// where a recording can be made and a replay can be substituted.
//
// A session is driven one machine frame at a time by the frontend, which is
// also where a breakpoint or a pause can leave a frame unfinished -- such a
// frame is dropped rather than recorded half-written.
class RzxSession : public IoReadHook {
public:
  enum class Mode { Off, Record, Play };

  RzxSession() = default;
  ~RzxSession() override;

  RzxSession(const RzxSession&) = delete;
  RzxSession& operator=(const RzxSession&) = delete;

  // Points the session at a machine and installs the port hook. A machine that
  // is replaced while a session runs must be bound again, so the frontend calls
  // this whenever it rebuilds one.
  void bind(Machine& machine);
  void unbind();

  Mode mode() const { return mode_; }
  bool recording() const { return mode_ == Mode::Record; }
  bool playing() const { return mode_ == Mode::Play; }
  bool busy() const { return mode_ != Mode::Off; }

  // --- recording ----------------------------------------------------------
  // Takes the snapshot that makes the recording replayable, so it belongs at a
  // frame boundary.
  bool startRecording(Machine& machine, std::string& error);
  // Hands the recording over and leaves the session off.
  bool takeRecording(rzx::File& out);
  // Throws the recording away.
  void cancel();

  // --- playback -----------------------------------------------------------
  // The caller has already applied the file's first snapshot, because that may
  // mean building a different machine.
  void startPlayback(rzx::File file);

  // One machine frame is over. Returns false when a replay has run out of
  // frames, in which case the session has already switched itself off.
  bool frameFinished(const Machine& machine);
  // The frame was cut short, by a breakpoint or a reset: drop what it captured.
  void frameAborted();

  u64 recordedFrames() const { return recorded_; }
  u64 playedFrames() const { return played_; }
  u64 totalFrames() const { return u64(frames_.size()); }
  // Reads that a replay had to invent, because the recording ran out of bytes
  // (or stored more than the machine asked for). Non-zero means the run is not
  // the one that was recorded.
  u64 desyncs() const { return desyncs_; }

  u8 ioRead(u16 port, u8 value) override;

private:
  void resetState();

  Mode mode_ = Mode::Off;
  Machine* machine_ = nullptr;

  rzx::File file_;
  std::vector<u8> frameInput_;
  u64 instructionsAtFrameStart_ = 0;
  u64 recorded_ = 0;

  // Playback walks a flattened list of frames: a repeat frame is stored as a
  // pointer to the frame it repeats, so the replay loop never has to look back.
  std::vector<const rzx::Frame*> frames_;
  size_t frameIndex_ = 0;
  size_t inputIndex_ = 0;
  u64 played_ = 0;
  u64 desyncs_ = 0;
};

} // namespace rainbow
