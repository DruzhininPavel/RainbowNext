#include "test_framework.h"

#include <string>
#include <vector>

#include "core/machine.h"
#include "core/tape.h"
#include "formats/tape.h"
#include "frontend/autoload.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::Tape;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;
using rainbow::formats::TapeBlock;
using rainbow::formats::TapeImage;

namespace {

void appendWord(std::vector<u8>& v, u16 value) {
  v.push_back(u8(value & 0xFF));
  v.push_back(u8(value >> 8));
}

void appendTriple(std::vector<u8>& v, u32 value) {
  v.push_back(u8(value & 0xFF));
  v.push_back(u8((value >> 8) & 0xFF));
  v.push_back(u8((value >> 16) & 0xFF));
}

// A TAP block is just a little-endian length followed by the payload.
void appendTapBlock(std::vector<u8>& tap, const std::vector<u8>& payload) {
  appendWord(tap, u16(payload.size()));
  tap.insert(tap.end(), payload.begin(), payload.end());
}

std::vector<u8> makeTzx(std::vector<u8> body) {
  std::vector<u8> tzx = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
  tzx.insert(tzx.end(), body.begin(), body.end());
  return tzx;
}

const std::vector<u8> kHeader = {0x00, 0x03, 'T', 'E', 'S', 'T', 0x00, 0x0A, 0x00, 0x0A, 0x00};
const std::vector<u8> kData = {0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};

} // namespace

TEST(tap_parses_blocks_with_standard_timings) {
  std::vector<u8> tap;
  appendTapBlock(tap, kHeader);
  appendTapBlock(tap, kData);

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));

  CHECK_EQ(image.blocks.size(), 2);
  CHECK_EQ(image.blocks[0].data.size(), kHeader.size());
  CHECK_EQ(image.blocks[0].data[0], 0x00);
  CHECK(image.blocks[0].isHeader());
  CHECK_EQ(image.blocks[1].data[0], 0xFF);
  CHECK(!image.blocks[1].isHeader());

  const TapeBlock& block = image.blocks[1];
  CHECK_EQ(block.pilotPulses, rainbow::formats::StandardPilotPulses);
  CHECK_EQ(block.pilotLength, rainbow::formats::StandardPilotLength);
  CHECK_EQ(block.syncFirst, rainbow::formats::StandardSyncFirst);
  CHECK_EQ(block.syncSecond, rainbow::formats::StandardSyncSecond);
  CHECK_EQ(block.zeroLength, rainbow::formats::StandardZeroLength);
  CHECK_EQ(block.oneLength, rainbow::formats::StandardOneLength);
  CHECK_EQ(block.pauseMs, rainbow::formats::StandardPauseMs);
}

TEST(tap_pulse_train_matches_the_rom_loader) {
  std::vector<u8> tap;
  appendTapBlock(tap, {0xFF, 0x00});

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));
  const TapeBlock& block = image.blocks[0];

  CHECK_EQ(block.bitCount(), 16);
  // Pilot tone, two sync pulses, two pulses per bit, one tail pulse.
  CHECK_EQ(block.pulseCount(), 8063 + 2 + 32 + 1);
  CHECK_EQ(block.pulseLength(0), 2168);
  CHECK_EQ(block.pulseLength(8062), 2168);
  CHECK_EQ(block.pulseLength(8063), 667);
  CHECK_EQ(block.pulseLength(8064), 735);
  // 0xFF is eight one bits, then 0x00 is eight zero bits.
  CHECK_EQ(block.pulseLength(8065), 1710);
  CHECK_EQ(block.pulseLength(8080), 1710);
  CHECK_EQ(block.pulseLength(8081), 855);
  CHECK_EQ(block.pulseLength(8096), 855);
  CHECK_EQ(block.pulseLength(8097), 1000);
}

TEST(tap_rejects_a_truncated_block) {
  std::vector<u8> tap;
  appendWord(tap, 16);
  tap.push_back(0xFF);
  tap.push_back(0x01);

  TapeImage image;
  std::string error;
  CHECK(!rainbow::formats::parseTap(tap, image, error));
  CHECK(!error.empty());
}

TEST(tzx_parses_standard_turbo_tone_and_pause_blocks) {
  std::vector<u8> body;
  body.push_back(0x30); // text description becomes the tape title
  body.push_back(8);
  const char* title = "TESTTAPE";
  body.insert(body.end(), title, title + 8);

  body.push_back(0x10); // standard speed data
  appendWord(body, 1000);
  appendTapBlock(body, kData);

  body.push_back(0x11); // turbo speed data
  appendWord(body, 2000); // pilot pulse
  appendWord(body, 500);  // sync first
  appendWord(body, 600);  // sync second
  appendWord(body, 750);  // zero
  appendWord(body, 1500); // one
  appendWord(body, 3223); // pilot pulses
  body.push_back(8);      // used bits in the last byte
  appendWord(body, 500);  // pause
  appendTriple(body, u32(kHeader.size()));
  body.insert(body.end(), kHeader.begin(), kHeader.end());

  body.push_back(0x12); // pure tone
  appendWord(body, 2168);
  appendWord(body, 100);

  body.push_back(0x20); // pause
  appendWord(body, 250);

  const std::vector<u8> tzx = makeTzx(body);
  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTzx(tzx, image, error));
  CHECK(image.fromTzx);
  CHECK(image.title == "TESTTAPE");
  CHECK_EQ(image.blocks.size(), 4);

  CHECK_EQ(image.blocks[0].kind, TapeBlock::Kind::Data);
  CHECK_EQ(image.blocks[0].pauseMs, 1000);
  CHECK_EQ(image.blocks[0].data.size(), kData.size());

  CHECK_EQ(image.blocks[1].kind, TapeBlock::Kind::Data);
  CHECK_EQ(image.blocks[1].pilotLength, 2000);
  CHECK_EQ(image.blocks[1].syncFirst, 500);
  CHECK_EQ(image.blocks[1].syncSecond, 600);
  CHECK_EQ(image.blocks[1].zeroLength, 750);
  CHECK_EQ(image.blocks[1].oneLength, 1500);
  CHECK_EQ(image.blocks[1].pilotPulses, 3223);
  CHECK_EQ(image.blocks[1].pauseMs, 500);

  CHECK_EQ(image.blocks[2].kind, TapeBlock::Kind::Pulses);
  CHECK_EQ(image.blocks[2].pulseCount(), 100);
  CHECK_EQ(image.blocks[2].pulseLength(7), 2168);

  CHECK_EQ(image.blocks[3].kind, TapeBlock::Kind::Pause);
  CHECK_EQ(image.blocks[3].pulseLength(0), 250 * 3500);
}

TEST(tzx_resolves_loops_and_jumps) {
  std::vector<u8> body;
  body.push_back(0x24); // loop start, three iterations
  appendWord(body, 3);
  body.push_back(0x12); // pure tone
  appendWord(body, 1000);
  appendWord(body, 5);
  body.push_back(0x25); // loop end

  body.push_back(0x23); // jump over the tone below
  const size_t jumpOffsetPosition = body.size();
  appendWord(body, 0);
  body.push_back(0x12); // skipped tone
  appendWord(body, 999);
  appendWord(body, 1);
  // The offset is relative to the first byte of the offset field itself.
  const u16 jumpOffset = u16(body.size()) - u16(jumpOffsetPosition);
  body[jumpOffsetPosition] = u8(jumpOffset & 0xFF);
  body[jumpOffsetPosition + 1] = u8(jumpOffset >> 8);

  body.push_back(0x10); // the block the jump lands on
  appendWord(body, 0);
  appendTapBlock(body, kData);

  const std::vector<u8> tzx = makeTzx(body);
  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTzx(tzx, image, error));
  CHECK_EQ(image.blocks.size(), 4);
  CHECK_EQ(image.blocks[0].kind, TapeBlock::Kind::Pulses);
  CHECK_EQ(image.blocks[2].kind, TapeBlock::Kind::Pulses);
  CHECK_EQ(image.blocks[3].kind, TapeBlock::Kind::Data);
}

TEST(tzx_rejects_unknown_blocks) {
  std::vector<u8> body;
  body.push_back(0x7F);
  const std::vector<u8> tzx = makeTzx(body);

  TapeImage image;
  std::string error;
  CHECK(!rainbow::formats::parseTzx(tzx, image, error));
  CHECK(!error.empty());
}

TEST(tape_playback_toggles_the_ear_bit) {
  std::vector<u8> tap;
  appendTapBlock(tap, kData);

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));

  Tape tape;
  tape.loadImage(image);
  CHECK(tape.loaded());
  CHECK(tape.ear()); // idle tape reads high, like the ULA's default

  tape.play();
  CHECK(tape.playing());

  tape.advance(2167);
  const bool firstLevel = tape.ear();
  tape.advance(1); // crosses into the second pilot pulse
  CHECK(tape.ear() != firstLevel);

  tape.advance(2168 * 100);
  CHECK(tape.playing());
  tape.stop();
  CHECK(!tape.playing());
}

TEST(tape_rewind_restarts_the_picture) {
  std::vector<u8> tap;
  appendTapBlock(tap, kData);
  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));

  Tape tape;
  tape.loadImage(image);
  tape.play();
  tape.advance(20000);
  CHECK(tape.blockIndex() >= 0);

  tape.rewind();
  CHECK_EQ(tape.blockIndex(), 0);
  CHECK(!tape.playing());
  CHECK(tape.ear());
}

// The interesting case: a real TAP served through the ROM's own LD-BYTES
// routine, exactly as a LOAD "" command would enter it.
TEST(tape_fast_load_serves_the_rom_loader) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;
  machine.reset();

  std::vector<u8> tap;
  appendTapBlock(tap, kData);

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));
  machine.tape().loadImage(image);
  machine.tape().setFastLoad(true);

  auto& regs = machine.cpu().regs();
  regs.a = 0xFF;      // the flag byte the caller expects
  regs.ix = 0x9000;   // destination
  regs.d = 0x00;
  regs.e = u8(kData.size() - 1);
  regs.sp = 0x7FFE;
  regs.pc = 0x0556;   // LD-BYTES
  machine.memWrite(0x7FFE, 0x34, 0);
  machine.memWrite(0x7FFF, 0x12, 0);

  machine.stepInstruction(); // the trap answers the call
  CHECK_EQ(regs.pc, 0x05DF);
  CHECK_EQ(regs.d, 0);
  CHECK_EQ(regs.e, 0);
  CHECK_EQ(regs.h, 0);
  CHECK_EQ(regs.ix, 0x9000 + kData.size() - 1);
  for (size_t i = 0; i < kData.size() - 1; ++i) {
    CHECK_EQ(machine.memRead(u16(0x9000 + i), 0), kData[1 + i]);
  }
  CHECK_EQ(machine.tape().blockIndex(), 1);

  machine.stepInstruction(); // LD A,H
  CHECK_EQ(regs.a, 0);
  machine.stepInstruction(); // CP 1 sets carry because the checksum was zero
  CHECK_EQ(regs.f & 0x01, 0x01);
  machine.stepInstruction(); // RET
  CHECK_EQ(regs.pc, 0x1234);
}

TEST(tape_fast_load_ignores_a_misaligned_block) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;
  machine.reset();

  std::vector<u8> tap;
  appendTapBlock(tap, kHeader); // a header block, but the caller wants data
  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));
  machine.tape().loadImage(image);

  auto& regs = machine.cpu().regs();
  regs.a = 0xFF;
  regs.ix = 0x9000;
  regs.d = 0x00;
  regs.e = 0x02;
  regs.pc = 0x0556;

  machine.stepInstruction();
  // Not handled: the ROM runs its own routine and the tape head stays put.
  CHECK(regs.pc != 0x05DF);
  CHECK_EQ(machine.tape().blockIndex(), 0);
}

// Custom loaders copy LD-BYTES' prologue and enter at 0x0562, leaving the
// expected flag byte in AF' for the ROM to swap back in at 0x05A9.
TEST(tape_fast_load_serves_a_loader_that_enters_at_0x0562) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;
  machine.reset();

  std::vector<u8> tap;
  appendTapBlock(tap, kData);

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));
  machine.tape().loadImage(image);
  machine.tape().setFastLoad(true);

  auto& regs = machine.cpu().regs();
  regs.a = 0x00;      // the live accumulator is stale on this entry
  regs.a2 = 0xFF;     // the flag byte lives in AF'
  regs.ix = 0x9000;
  regs.d = 0x00;
  regs.e = u8(kData.size() - 1);
  regs.sp = 0x7FFE;
  regs.pc = 0x0562;
  machine.memWrite(0x7FFE, 0x34, 0);
  machine.memWrite(0x7FFF, 0x12, 0);

  machine.stepInstruction();
  CHECK_EQ(regs.pc, 0x05DF);
  CHECK_EQ(regs.d, 0);
  CHECK_EQ(regs.e, 0);
  CHECK_EQ(regs.h, 0);
  CHECK_EQ(regs.ix, 0x9000 + kData.size() - 1);
  for (size_t i = 0; i < kData.size() - 1; ++i) {
    CHECK_EQ(machine.memRead(u16(0x9000 + i), 0), kData[1 + i]);
  }
  CHECK_EQ(machine.tape().blockIndex(), 1);

  machine.stepInstruction(); // LD A,H
  machine.stepInstruction(); // CP 1
  CHECK_EQ(regs.f & 0x01, 0x01);
  machine.stepInstruction(); // RET, straight back to the loader's caller
  CHECK_EQ(regs.pc, 0x1234);
}

namespace {

void tapKey(Machine& machine, int row, int column) {
  machine.keyboard().setKey(row, column, true);
  for (int i = 0; i < 4; ++i) machine.runFrame();
  machine.keyboard().setKey(row, column, false);
  for (int i = 0; i < 4; ++i) machine.runFrame();
}

void tapSymbolShiftKey(Machine& machine, int row, int column) {
  machine.keyboard().setKey(7, 1, true); // SYMBOL SHIFT
  for (int i = 0; i < 4; ++i) machine.runFrame();
  tapKey(machine, row, column);
  machine.keyboard().setKey(7, 1, false);
  for (int i = 0; i < 4; ++i) machine.runFrame();
}

// Recovers the text of one character row by matching each cell against the ROM
// character set, so the test does not depend on how glyphs are rasterised.
std::string screenRowText(Machine& machine, int textRow) {
  constexpr u16 kChars = 0x3D00;
  std::string text;
  for (int column = 0; column < 32; ++column) {
    const int y = textRow * 8;
    const u16 address = u16(0x4000 + ((y & 0xC0) << 5) + ((y & 0x07) << 8) +
                            ((y & 0x38) << 2) + column);
    int bestChar = '?';
    int bestDistance = 8 * 8 + 1;
    for (int ch = 32; ch < 128; ++ch) {
      int distance = 0;
      for (int line = 0; line < 8; ++line) {
        const u8 glyph = machine.memRead(u16(kChars + u16((ch - 32) * 8 + line)), 0);
        const u8 pixels = machine.memRead(u16(address + u16(line << 8)), 0);
        distance += __builtin_popcount(pixels ^ glyph);
      }
      if (distance < bestDistance) {
        bestDistance = distance;
        bestChar = ch;
      }
    }
    text += char(bestChar);
  }
  return text;
}

// Builds a TAP holding a single BASIC line, `10 PRINT "HELLO"`.
std::vector<u8> makeHelloTap() {
  const std::vector<u8> program = {0x00, 0x0A, 0x00, 0x0B, 0xF5, 0x22, 'H', 'E',
                                   'L',  'L',  'O',  0x22, 0x0D, 0x80};

  const auto withChecksum = [](std::vector<u8> payload) {
    u8 sum = 0;
    for (u8 byte : payload) sum ^= byte;
    payload.push_back(sum);
    return payload;
  };

  // TAP header payload: flag, type (0 = program), ten-character name, length,
  // autostart line and program length; the checksum follows.
  std::vector<u8> header = {0x00, 0x00, 'H', 'E', 'L', 'L', 'O', ' ', ' ', ' ', ' ', ' '};
  appendWord(header, u16(program.size())); // length
  appendWord(header, 10);                  // autostart line
  appendWord(header, u16(program.size())); // program length without variables

  std::vector<u8> data = {0xFF};
  data.insert(data.end(), program.begin(), program.end());

  std::vector<u8> tap;
  appendTapBlock(tap, withChecksum(header));
  appendTapBlock(tap, withChecksum(data));
  return tap;
}

} // namespace

// Full path through the real ROM: `LOAD ""` must reach the LD-BYTES trap and
// leave the BASIC program in memory exactly as it is on the tape.
TEST(rom_loads_a_basic_program_from_a_tap) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;

  const std::vector<u8> tap = makeHelloTap();
  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(tap, image, error));
  image.title = "hello.tap";
  machine.tape().loadImage(image);

  for (int i = 0; i < 130; ++i) machine.runFrame();

  tapKey(machine, 6, 3); // J -> LOAD keyword
  tapSymbolShiftKey(machine, 5, 0); // P with SYMBOL SHIFT -> "
  tapSymbolShiftKey(machine, 5, 0);
  tapKey(machine, 6, 0); // ENTER

  for (int i = 0; i < 40; ++i) machine.runFrame();

  const u16 program = u16(machine.memRead(0x5C53, 0)) | u16(machine.memRead(0x5C54, 0) << 8);
  CHECK_EQ(program, 0x5CCB);

  const std::vector<u8> expected = {0x00, 0x0A, 0x00, 0x0B, 0xF5, 0x22, 'H', 'E',
                                    'L',  'L',  'O',  0x22, 0x0D, 0x80};
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK_EQ(machine.memRead(u16(program + i), 0), expected[i]);
  }
  CHECK_EQ(machine.tape().blockIndex(), machine.tape().blockCount());

  // The header carries autostart line 10, so the ROM announces the program and
  // runs it: that proves the loaded bytes are a real listing, not just RAM.
  CHECK(screenRowText(machine, 1).rfind("Program: HELLO", 0) == 0);
  CHECK(screenRowText(machine, 2).rfind("HELLO", 0) == 0);
}

// The Autoload helper is what the frontend uses for `--tape` and the Tape menu,
// so it is checked against the same real ROM the test above drives by hand: the
// only difference is that the key presses come from the sequence instead of
// from the test.
TEST(autoload_types_the_load_command_for_a_48k_machine) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;

  TapeImage image;
  std::string error;
  CHECK(rainbow::formats::parseTap(makeHelloTap(), image, error));
  image.title = "hello.tap";
  machine.tape().loadImage(image);

  rainbow::frontend::Autoload autoload;
  autoload.start(machine);
  CHECK(autoload.active());
  while (autoload.active()) {
    machine.runFrame();
    autoload.tick();
  }
  CHECK(!autoload.active());
  // The sequence starts the deck on its own, because the ROM is only listening
  // for pulses once LOAD "" has been entered.
  CHECK(machine.tape().playing());

  for (int i = 0; i < 40; ++i) machine.runFrame();

  const u16 program = u16(machine.memRead(0x5C53, 0)) | u16(machine.memRead(0x5C54, 0) << 8);
  CHECK_EQ(program, 0x5CCB);
}

TEST(autoload_cancel_stops_the_sequence) {
  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return;

  rainbow::frontend::Autoload autoload;
  autoload.start(machine);
  // Step up to the first key press, which is the LOAD keyword.
  for (int i = 0; i < 400 && !machine.keyboard().isDown(6, 3); ++i) {
    machine.runFrame();
    autoload.tick();
  }
  CHECK_EQ(machine.keyboard().isDown(6, 3), true);

  autoload.cancel();
  CHECK(!autoload.active());
  // Cancelling must not leave a key stuck down.
  CHECK_EQ(machine.keyboard().isDown(6, 3), false);
}
