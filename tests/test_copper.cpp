// The Copper coprocessor.
//
// The Copper owns 2K of program memory, addressed as 1024 two-byte
// instructions. A WAIT instruction blocks until its raster line comes round, a
// MOVE instruction writes one Next register, and a program that waits for a
// line past the end of the frame halts. Because the compositor samples the
// register file once per scanline, a MOVE shows up on the line it was made for.

#include <vector>

#include "test_framework.h"

#include "core/copper.h"
#include "core/machine.h"

using mynext::Copper;
using mynext::Machine;
using mynext::MachineType;
using mynext::u16;
using mynext::u32;
using mynext::u8;

namespace {

u16 move(u8 reg, u8 value) { return u16((u16(reg & 0x7F) << 8) | value); }
u16 waitLine(int line, int hpos = 0) {
  return u16(0x8000 | ((hpos & 0x3F) << 9) | (line & 0x1FF));
}

// Records the writes the Copper performs, so a test can look at them in order.
struct Recorder {
  std::vector<u16> writes;

  void operator()(u8 reg, u8 value) { writes.push_back(u16((u16(reg) << 8) | value)); }
};

void loadWords(Copper& copper, const std::vector<u16>& words) {
  copper.writeAddressLsb(0x00);
  copper.writeControl(0x00);
  for (u16 word : words) {
    copper.writeData16(u8(word >> 8));
    copper.writeData16(u8(word & 0xFF));
  }
}

// Loads a program through the Next register file, the way a real one is
// uploaded: 0x63 takes the high byte then the low byte of each instruction.
void loadThroughRegisters(Machine& m, const std::vector<u16>& words) {
  m.writeNextReg(0x61, 0x00);
  m.writeNextReg(0x62, 0x00);
  for (u16 word : words) {
    m.writeNextReg(0x63, u8(word >> 8));
    m.writeNextReg(0x63, u8(word & 0xFF));
  }
}

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

// ULA palette entry 16 is the border colour, so this is the colour a frame's
// border takes when entry 16 holds `value`.
u32 borderColour(u8 value) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x40, 0x10);
  m.writeNextReg(0x41, value);
  renderOneFrame(m);
  return surfacePixel(m, 0, 0);
}

// A program that paints the border one colour, waits, then repaints it.
std::vector<u16> borderRepaintProgram(int line, u8 before, u8 after) {
  return {
      move(0x40, 0x10), // palette index 16
      move(0x41, before),
      waitLine(line),
      move(0x40, 0x10),
      move(0x41, after),
      0xFFFF, // halt
  };
}

} // namespace

// ---------------------------------------------------------------------------
// Program memory
// ---------------------------------------------------------------------------

TEST(copper_data8_fills_the_program_and_wraps_at_the_end) {
  Copper copper;
  copper.writeAddressLsb(0xFF);
  copper.writeControl(0x07); // index 0x7FF, the last byte of the program

  copper.writeData8(0x11);
  CHECK_EQ(copper.instruction(1023) & 0xFF, 0x11);
  copper.writeData8(0x22);
  // The index wrapped past the end of the program back to its first byte.
  CHECK_EQ(copper.addressLsb(), 0x01);
  CHECK_EQ(copper.instruction(0) >> 8, 0x22);
}

TEST(copper_data16_commits_a_word_only_after_its_second_byte) {
  Copper copper;
  copper.writeAddressLsb(0x00);
  copper.writeControl(0x00);

  copper.writeData16(0x15);
  CHECK_EQ(copper.instruction(0), 0x0000); // high byte parked, not committed
  copper.writeData16(0x03);
  CHECK_EQ(copper.instruction(0), 0x1503);
}

TEST(copper_address_and_control_registers_read_back) {
  Copper copper;
  copper.writeAddressLsb(0x34);
  CHECK_EQ(copper.addressLsb(), 0x34);
  CHECK_EQ(copper.control(), 0x00); // mode 0, top bits clear

  copper.writeControl(0x02); // index 0x234, still stopped
  CHECK_EQ(copper.control(), 0x02);
  CHECK_EQ(copper.mode(), 0);
  CHECK(!copper.running());
  CHECK_EQ(copper.addressLsb(), 0x34);

  copper.writeControl(0x42); // mode 1
  CHECK_EQ(copper.control(), 0x42);
  CHECK(copper.running());
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

TEST(copper_move_writes_its_register) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x55), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 1u);
  CHECK_EQ(recorder.writes[0], 0x1455);
}

TEST(copper_noop_does_nothing) {
  Copper copper;
  loadWords(copper, {0x0000, 0x0000, move(0x15, 0x03), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 1u);
  CHECK_EQ(recorder.writes[0], 0x1503);
}

TEST(copper_wait_blocks_until_its_line) {
  Copper copper;
  loadWords(copper, {waitLine(50), move(0x15, 0x03), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 0u);
  CHECK_EQ(copper.waitLine(), 50);

  copper.runToLine(49, recorder);
  CHECK_EQ(recorder.writes.size(), 0u);

  copper.runToLine(50, recorder);
  CHECK_EQ(recorder.writes.size(), 1u);
  CHECK_EQ(recorder.writes[0], 0x1503);
}

TEST(copper_wait_records_its_horizontal_position) {
  Copper copper;
  loadWords(copper, {waitLine(80, 12), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(10, recorder);
  CHECK_EQ(copper.waitLine(), 80);
  CHECK_EQ(copper.waitHpos(), 12);
}

TEST(copper_halt_waits_for_a_line_past_the_end_of_the_frame) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x01), 0xFFFF, move(0x14, 0x02)});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 1u);

  // 0xFFFF is a wait for line 511, which never arrives, so nothing after it
  // ever runs.
  for (int line = 0; line < 312; ++line) copper.runToLine(line, recorder);
  CHECK_EQ(recorder.writes.size(), 1u);
}

TEST(copper_a_program_without_waits_cannot_stall_the_frame) {
  Copper copper;
  std::vector<u16> program;
  for (int i = 0; i < 1100; ++i) program.push_back(move(0x14, u8(i)));
  loadWords(copper, program);
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 1024u); // one pass, then the line ends
}

TEST(copper_stopped_mode_runs_nothing) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x01), 0xFFFF});
  CHECK_EQ(copper.mode(), 0);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 0u);
  CHECK_EQ(copper.programCounter(), 0);
}

// ---------------------------------------------------------------------------
// Control modes
// ---------------------------------------------------------------------------

TEST(copper_mode_one_restarts_from_the_first_instruction) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x01), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(copper.programCounter(), 1); // parked on the halt

  copper.writeControl(0x00); // stop, keeping the counter
  CHECK_EQ(copper.programCounter(), 1);
  copper.writeControl(0x40); // start again from the top
  CHECK_EQ(copper.programCounter(), 0);
}

TEST(copper_mode_two_continues_from_the_current_instruction) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x01), move(0x14, 0x02), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  copper.writeControl(0x00);
  CHECK_EQ(copper.programCounter(), 2);

  copper.writeControl(0x80); // mode 2
  CHECK_EQ(copper.programCounter(), 2);

  // Restarting from the top instead replays both moves.
  copper.writeControl(0x00);
  copper.writeControl(0x40);
  recorder.writes.clear();
  copper.runToLine(0, recorder);
  CHECK_EQ(recorder.writes.size(), 2u);
}

TEST(copper_repeating_the_mode_only_moves_the_write_index) {
  Copper copper;
  loadWords(copper, {move(0x14, 0x01), 0xFFFF});
  copper.writeControl(0x40);

  Recorder recorder;
  copper.runToLine(0, recorder);
  CHECK_EQ(copper.programCounter(), 1);

  // Uploading more of the program with the mode already at 01 must not
  // restart the running program.
  copper.writeControl(0x42); // index high bits = 2, same mode
  CHECK_EQ(copper.programCounter(), 1);
  CHECK_EQ(copper.control(), 0x42);
}

// ---------------------------------------------------------------------------
// Register file and rendering
// ---------------------------------------------------------------------------

TEST(copper_registers_are_reachable_through_the_next_register_file) {
  Machine m(MachineType::Next);
  loadThroughRegisters(m, {move(0x15, 0x03), 0xFFFF});

  m.writeNextReg(0x62, 0x40); // mode 1
  renderOneFrame(m);

  CHECK_EQ(m.readNextReg(0x15), 0x03);
  CHECK_EQ(m.copper().programCounter(), 1);
}

TEST(copper_changes_the_border_colour_in_the_middle_of_a_frame) {
  Machine m(MachineType::Next);
  loadThroughRegisters(m, borderRepaintProgram(150, 0x00, 0xE3));
  m.writeNextReg(0x62, 0x40);

  renderOneFrame(m);

  // Raster line 150 falls inside the visible area, so the top border keeps the
  // colour the program set first and the bottom border gets the second one.
  CHECK_EQ(surfacePixel(m, 0, 0), borderColour(0x00));
  CHECK_EQ(surfacePixel(m, 0, Machine::FrameHeight - 1), borderColour(0xE3));
}

TEST(copper_mode_three_replays_its_program_on_every_frame) {
  Machine m(MachineType::Next);
  loadThroughRegisters(m, borderRepaintProgram(150, 0x00, 0xE3));
  m.writeNextReg(0x62, 0xC0); // mode 3

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, Machine::FrameHeight - 1), borderColour(0xE3));

  // Undo the program's work by hand: only a program that starts over can put
  // the colour back.
  m.writeNextReg(0x40, 0x10);
  m.writeNextReg(0x41, 0x00);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, Machine::FrameHeight - 1), borderColour(0xE3));
}

TEST(copper_mode_two_does_not_replay_its_program) {
  Machine m(MachineType::Next);
  loadThroughRegisters(m, borderRepaintProgram(150, 0x00, 0xE3));
  m.writeNextReg(0x62, 0x80); // mode 2: no per-frame restart

  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, Machine::FrameHeight - 1), borderColour(0xE3));

  m.writeNextReg(0x40, 0x10);
  m.writeNextReg(0x41, 0x00);
  renderOneFrame(m);
  CHECK_EQ(surfacePixel(m, 0, Machine::FrameHeight - 1), borderColour(0x00));
}
