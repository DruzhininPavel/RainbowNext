#include "test_framework.h"

#include <string>
#include <vector>

#include "core/machine.h"
#include "core/rzx.h"
#include "core/rzx_session.h"

namespace {

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::RzxSession;
using rainbow::rzx::Block;
using rainbow::rzx::File;
using rainbow::rzx::Frame;
using rainbow::rzx::InputBlock;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;

void loadProgram(Machine& machine, u16 address, std::initializer_list<u8> code) {
  u16 at = address;
  for (u8 byte : code) machine.memWrite(at++, byte, 0);
  machine.cpu().regs().pc = address;
}

void append(std::vector<u8>& out, std::initializer_list<u8> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

// A file built by hand exactly the way libspectrum builds one, so the parser is
// pinned to the real layout and not to whatever our own writer happens to emit.
std::vector<u8> libspectrumStyleFile() {
  std::vector<u8> data;
  append(data, {'R', 'Z', 'X', '!', 0, 12, 0, 0, 0, 0});

  // Creator: 20 bytes of program name, then two words. 29 bytes with the header.
  append(data, {0x10, 29, 0, 0, 0});
  append(data, {'T', 'e', 's', 't'});
  for (int i = 0; i < 16; ++i) data.push_back(0);
  append(data, {0x03, 0x00, 0x07, 0x00});

  // Snapshot: flags, a four-character format, the uncompressed length, data.
  append(data, {0x30, 21, 0, 0, 0});
  append(data, {0, 0, 0, 0});
  append(data, {'S', 'N', 'A', 0});
  append(data, {4, 0, 0, 0});
  append(data, {0xDE, 0xAD, 0xBE, 0xEF});

  // Input: two frames, the second of which repeats the first.
  append(data, {0x80, 29, 0, 0, 0});
  append(data, {2, 0, 0, 0});   // frame count
  data.push_back(0);            // undefined frame size
  append(data, {0x40, 0x1F, 0, 0});  // T-states
  append(data, {0, 0, 0, 0});   // flags
  append(data, {0x0A, 0x00, 0x03, 0x00, 0xAA, 0xBB, 0xCC});
  append(data, {0x0B, 0x00, 0xFF, 0xFF});
  return data;
}

TEST(rzx_parses_the_libspectrum_layout) {
  File file;
  std::string error;
  CHECK(rainbow::rzx::parse(libspectrumStyleFile(), file, error));

  CHECK(file.creator.program == "Test");
  CHECK_EQ(file.creator.major, 3);
  CHECK_EQ(file.creator.minor, 7);

  CHECK_EQ(file.blocks.size(), 2u);
  CHECK(file.blocks[0].kind == Block::Kind::Snapshot);
  CHECK(file.blocks[0].snapshot.format == "SNA");
  CHECK_EQ(file.blocks[0].snapshot.data.size(), 4u);
  CHECK_EQ(file.blocks[0].snapshot.data[0], 0xDE);

  const InputBlock& input = file.blocks[1].input;
  CHECK_EQ(input.tstates, 0x1F40);
  CHECK_EQ(input.frames.size(), 2u);
  CHECK_EQ(input.frames[0].instructions, 10);
  CHECK_EQ(input.frames[0].input.size(), 3u);
  CHECK_EQ(input.frames[0].input[2], 0xCC);
  CHECK(input.frames[1].repeat);
}

TEST(rzx_round_trip_keeps_every_block) {
  File original;
  original.creator = {"RainbowNext", 0, 1};
  Block snapshot;
  snapshot.kind = Block::Kind::Snapshot;
  snapshot.snapshot.format = "RBNS";
  snapshot.snapshot.data = {1, 2, 3, 4, 5, 6, 7};
  original.blocks.push_back(snapshot);

  InputBlock& input = original.appendInput(69888);
  Frame first;
  first.instructions = 1234;
  first.input = {0xFE, 0x1F, 0x00};
  input.frames.push_back(first);
  Frame second;
  second.instructions = 999;
  second.repeat = true;
  input.frames.push_back(second);

  std::vector<u8> bytes;
  rainbow::rzx::write(original, bytes);

  File parsed;
  std::string error;
  CHECK(rainbow::rzx::parse(bytes, parsed, error));
  CHECK(parsed.creator.program == "RainbowNext");
  CHECK_EQ(parsed.blocks.size(), 2u);
  CHECK(parsed.blocks[0].snapshot.format == "RBNS");
  CHECK_EQ(parsed.blocks[0].snapshot.data.size(), 7u);
  CHECK_EQ(parsed.blocks[1].input.tstates, 69888);
  CHECK_EQ(parsed.blocks[1].input.frames.size(), 2u);
  CHECK_EQ(parsed.blocks[1].input.frames[0].instructions, 1234);
  CHECK_EQ(parsed.blocks[1].input.frames[0].input.size(), 3u);
  CHECK_EQ(parsed.blocks[1].input.frames[0].input[1], 0x1F);
  CHECK(parsed.blocks[1].input.frames[1].repeat);
}

TEST(rzx_rejects_a_file_that_is_not_rzx) {
  File file;
  std::string error;
  std::vector<u8> data = {'Z', '8', '0', '!', 0, 0, 0, 0, 0, 0};
  CHECK(!rainbow::rzx::parse(data, file, error));
  CHECK(!error.empty());
}

TEST(rzx_rejects_a_truncated_block) {
  std::vector<u8> data = libspectrumStyleFile();
  data.resize(data.size() - 3);
  File file;
  std::string error;
  CHECK(!rainbow::rzx::parse(data, file, error));
  CHECK(!error.empty());
}

TEST(rzx_skips_blocks_it_does_not_know) {
  std::vector<u8> data = libspectrumStyleFile();
  // A signature block sits between the header and the creator; 13 bytes is the
  // smallest one the format allows.
  std::vector<u8> signed_file(data.begin(), data.begin() + 10);
  append(signed_file, {0x20, 13, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8});
  signed_file.insert(signed_file.end(), data.begin() + 10, data.end());

  File file;
  std::string error;
  CHECK(rainbow::rzx::parse(signed_file, file, error));
  CHECK_EQ(file.blocks.size(), 2u);
  CHECK(file.creator.program == "Test");
}

// A recording is the bytes the ports returned. Making one must not change what
// the machine sees, and replaying it must reproduce those bytes even when the
// machine would answer differently.
TEST(rzx_recording_captures_the_port_reads) {
  Machine machine(MachineType::Spectrum48);
  RzxSession session;
  session.bind(machine);

  std::string error;
  CHECK(session.startRecording(machine, error));
  CHECK(session.recording());

  std::vector<u8> seen;
  for (int i = 0; i < 5; ++i) seen.push_back(machine.ioRead(0x00FE, 0));
  CHECK(session.frameFinished(machine));
  CHECK_EQ(session.recordedFrames(), 1u);

  File file;
  CHECK(session.takeRecording(file));
  CHECK(!session.busy());

  CHECK_EQ(file.blocks.size(), 2u);
  CHECK(file.blocks[0].kind == Block::Kind::Snapshot);
  CHECK(file.blocks[0].snapshot.format == "RBNS");
  CHECK(!file.blocks[0].snapshot.data.empty());
  CHECK_EQ(file.blocks[1].input.frames.size(), 1u);
  CHECK_EQ(file.blocks[1].input.frames[0].input.size(), 5u);
  for (size_t i = 0; i < seen.size(); ++i) {
    CHECK_EQ(file.blocks[1].input.frames[0].input[i], seen[i]);
  }
}

TEST(rzx_replay_feeds_the_recorded_bytes_back) {
  Machine recorder(MachineType::Spectrum48);
  RzxSession recording;
  recording.bind(recorder);

  std::string error;
  CHECK(recording.startRecording(recorder, error));
  // The ULA read carries the keyboard, so a held key makes the recording
  // distinguishable from what an untouched machine would answer.
  recorder.keyboard().setKey(0, 0, true);
  std::vector<u8> seen;
  for (int i = 0; i < 4; ++i) seen.push_back(recorder.ioRead(0x00FE, 0));
  CHECK(recording.frameFinished(recorder));

  File file;
  CHECK(recording.takeRecording(file));
  recording.unbind();

  Machine player(MachineType::Spectrum48);
  RzxSession playback;
  playback.bind(player);
  // No key held this time: only the recording can produce the same bytes.
  const u8 untouched = player.ioRead(0x00FE, 0);
  CHECK(untouched != seen[0]);

  playback.startPlayback(std::move(file));
  CHECK(playback.playing());
  for (int i = 0; i < 4; ++i) CHECK_EQ(player.ioRead(0x00FE, 0), seen[i]);
  CHECK(!playback.frameFinished(player));
  CHECK_EQ(playback.playedFrames(), 1u);
  CHECK_EQ(playback.desyncs(), 0u);
}

// The whole point: record a running machine, then start a fresh one from the
// snapshot inside the file and run it on the recording alone.
TEST(rzx_replay_runs_the_recorded_program) {
  Machine recorder(MachineType::Spectrum48);
  // Read the ULA, store it, and loop, so the RAM image ends up depending on the
  // input stream.
  loadProgram(recorder, 0x8000, {0xDB, 0xFE, 0x32, 0x00, 0x90, 0xC3, 0x00, 0x80});
  recorder.keyboard().setKey(1, 2, true);

  RzxSession session;
  session.bind(recorder);
  std::string error;
  CHECK(session.startRecording(recorder, error));
  CHECK(!recorder.runFrame().has_value());
  CHECK(session.frameFinished(recorder));
  CHECK(!recorder.runFrame().has_value());
  CHECK(session.frameFinished(recorder));
  const u8 recordedLast = recorder.memRead(0x9000, 0);

  File file;
  CHECK(session.takeRecording(file));
  CHECK_EQ(file.blocks.size(), 2u);
  CHECK_EQ(file.blocks[1].input.frames.size(), 2u);
  CHECK(file.blocks[1].input.frames[0].input.size() > 0);
  session.unbind();

  std::vector<u8> bytes;
  rainbow::rzx::write(file, bytes);

  File parsed;
  CHECK(rainbow::rzx::parse(bytes, parsed, error));
  CHECK(parsed.blocks[0].kind == Block::Kind::Snapshot);
  CHECK(parsed.blocks[0].snapshot.format == "RBNS");

  Machine player(MachineType::Spectrum48);
  CHECK(player.loadState(parsed.blocks[0].snapshot.data, error));

  RzxSession playback;
  playback.bind(player);
  playback.startPlayback(std::move(parsed));
  CHECK_EQ(playback.totalFrames(), 2u);

  CHECK(!player.runFrame().has_value());
  CHECK(playback.frameFinished(player));
  CHECK(!player.runFrame().has_value());
  CHECK(!playback.frameFinished(player));
  CHECK_EQ(playback.desyncs(), 0u);
  CHECK_EQ(player.memRead(0x9000, 0), recordedLast);
}

// End to end on the real ROM: a replay that drifts would show up as a different
// screen, so comparing all of RAM is a much stronger check than a hash of the
// registers.
TEST(rzx_replay_reproduces_a_real_rom_boot) {
  const std::string rom = std::string(RAINBOW_SOURCE_DIR) + "/roms/48.rom";
  Machine recorder(MachineType::Spectrum48);
  if (!recorder.loadRomImage(rom)) return;
  recorder.reset();
  // A key held during the boot changes what the ROM reads back from the ULA, so
  // the recording carries information a fresh machine cannot invent.
  recorder.keyboard().setKey(3, 0, true);

  RzxSession session;
  session.bind(recorder);
  std::string error;
  CHECK(session.startRecording(recorder, error));
  for (int i = 0; i < 80; ++i) {
    CHECK(!recorder.runFrame().has_value());
    CHECK(session.frameFinished(recorder));
  }

  File file;
  CHECK(session.takeRecording(file));
  session.unbind();

  std::vector<u8> bytes;
  rainbow::rzx::write(file, bytes);
  File parsed;
  CHECK(rainbow::rzx::parse(bytes, parsed, error));
  CHECK(parsed.blocks[0].kind == Block::Kind::Snapshot);

  Machine player(MachineType::Spectrum48);
  CHECK(player.loadRomImage(rom));
  CHECK(player.loadState(parsed.blocks[0].snapshot.data, error));

  RzxSession playback;
  playback.bind(player);
  playback.startPlayback(std::move(parsed));
  CHECK_EQ(playback.totalFrames(), 80u);
  for (int i = 0; i < 80; ++i) {
    CHECK(!player.runFrame().has_value());
    playback.frameFinished(player);
  }
  CHECK_EQ(playback.desyncs(), 0u);

  int differences = 0;
  for (u32 address = 0x4000; address <= 0xFFFF; ++address) {
    if (recorder.memRead(u16(address), 0) != player.memRead(u16(address), 0)) ++differences;
  }
  CHECK_EQ(differences, 0);
}

TEST(rzx_a_dropped_frame_is_not_recorded) {  Machine machine(MachineType::Spectrum48);
  RzxSession session;
  session.bind(machine);
  std::string error;
  CHECK(session.startRecording(machine, error));
  machine.ioRead(0x00FE, 0);
  session.frameAborted();
  CHECK(session.frameFinished(machine));

  File file;
  CHECK(session.takeRecording(file));
  CHECK_EQ(file.blocks[1].input.frames.size(), 1u);
  CHECK_EQ(file.blocks[1].input.frames[0].input.size(), 0u);
}

} // namespace
