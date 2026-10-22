#include "test_framework.h"

#include "core/machine.h"
#include "core/rewind.h"
#include "core/state.h"

namespace {

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::RewindBuffer;
using rainbow::u16;
using rainbow::u32;
using rainbow::u64;
using rainbow::u8;

void loadProgram(Machine& machine, u16 address, std::initializer_list<u8> code) {
  u16 at = address;
  for (u8 byte : code) machine.memWrite(at++, byte, 0);
  machine.cpu().regs().pc = address;
}

std::vector<u8> saveToBytes(Machine& machine) {
  std::vector<u8> state;
  machine.saveState(state);
  return state;
}

// A state that a machine wrote must load into another machine of the same type
// and leave it in the same place, which is the whole point of a save state.
TEST(state_round_trip_restores_the_cpu) {
  Machine a(MachineType::Spectrum128);
  loadProgram(a, 0xC000, {0x3E, 0x2A, 0x06, 0x07, 0xC3, 0x00, 0xC0});
  a.stepInstruction();
  a.stepInstruction();
  const u8 aValue = a.cpu().regs().a;
  const u8 bValue = a.cpu().regs().b;
  const u16 pc = a.cpu().regs().pc;
  CHECK_EQ(aValue, 0x2A);
  CHECK_EQ(bValue, 0x07);
  const std::vector<u8> state = saveToBytes(a);

  Machine b(MachineType::Spectrum128);
  std::string error;
  CHECK(b.loadState(state, error));
  CHECK(error.empty());
  CHECK_EQ(b.cpu().regs().a, aValue);
  CHECK_EQ(b.cpu().regs().b, bValue);
  CHECK_EQ(b.cpu().regs().pc, pc);
  CHECK_EQ(b.cpu().regs().sp, a.cpu().regs().sp);
  CHECK_EQ(b.cpu().regs().iff1, a.cpu().regs().iff1);
  CHECK_EQ(b.tstatesPerFrameValue(), a.tstatesPerFrameValue());
}

TEST(state_round_trip_restores_memory) {
  Machine a(MachineType::Spectrum128);
  a.memWrite(0x8000, 0x11, 0);
  a.memWrite(0xFFFF, 0x99, 0);
  const std::vector<u8> state = saveToBytes(a);

  Machine b(MachineType::Spectrum128);
  std::string error;
  CHECK(b.loadState(state, error));
  CHECK_EQ(b.memRead(0x8000, 0), 0x11);
  CHECK_EQ(b.memRead(0xFFFF, 0), 0x99);
}

// Paging, the border and the AY registers are not derivable from RAM, so a
// state that forgot them would load into a machine showing the wrong screen.
TEST(state_round_trip_restores_paging_and_peripherals) {
  Machine a(MachineType::Spectrum128);
  a.ioWrite(0x7FFD, 0x13, 0); // ROM 1, bank 3 at 0xC000, screen at bank 7
  a.ioWrite(0xFE, 0x02, 0);   // red border
  a.ay().select(1);
  a.ay().write(0x55);
  const u8 port7ffd = a.port7ffd();
  const u8 border = a.border();
  CHECK_EQ(port7ffd, 0x13);
  CHECK_EQ(border, 0x02);

  const std::vector<u8> state = saveToBytes(a);
  Machine b(MachineType::Spectrum128);
  std::string error;
  CHECK(b.loadState(state, error));
  CHECK_EQ(b.port7ffd(), port7ffd);
  CHECK_EQ(b.ramBank(), a.ramBank());
  CHECK_EQ(b.screenBank(), a.screenBank());
  CHECK_EQ(b.border(), border);
  CHECK_EQ(b.ay().read(), a.ay().read());
}

// The Next keeps its memory map in the MMU registers, so the resolved page
// table has to be rebuilt from them rather than stored.
TEST(state_round_trip_restores_the_next_mmu) {
  Machine a(MachineType::Next);
  a.writeNextReg(0x50, 10);
  a.writeNextReg(0x51, 11);
  a.writeNextReg(0x52, 12);
  a.writeNextReg(0x53, 13);
  a.writeNextReg(0x43, 0x10);
  a.writeNextReg(0x40, 0x05);
  a.writeNextReg(0x41, 0xE3);
  const u32 colour = a.palette().colour(2, 5);

  const std::vector<u8> state = saveToBytes(a);
  Machine b(MachineType::Next);
  std::string error;
  CHECK(b.loadState(state, error));
  CHECK_EQ(b.nextRegs().mmuSlot(0), 10);
  CHECK_EQ(b.nextRegs().mmuSlot(3), 13);
  CHECK_EQ(b.palette().colour(2, 5), colour);
}

// A state is a file that outlives the build that wrote it, so every mismatch
// that would run the wrong code has to be refused rather than applied.
TEST(state_refuses_a_different_machine) {
  Machine a(MachineType::Spectrum128);
  const std::vector<u8> state = saveToBytes(a);
  Machine b(MachineType::Spectrum48);
  std::string error;
  CHECK(!b.loadState(state, error));
  CHECK(error.find("Spectrum") != std::string::npos);
}

TEST(state_refuses_a_damaged_file) {
  Machine a(MachineType::Spectrum128);
  std::vector<u8> state = saveToBytes(a);
  // Flip a byte in the middle of the payload, where a checksum is the only
  // thing that can notice.
  state[state.size() / 2] ^= 0xFF;
  Machine b(MachineType::Spectrum128);
  std::string error;
  CHECK(!b.loadState(state, error));
  CHECK(error.find("damaged") != std::string::npos);
}

TEST(state_refuses_a_truncated_file) {
  Machine a(MachineType::Spectrum128);
  const std::vector<u8> state = saveToBytes(a);
  Machine b(MachineType::Spectrum128);
  std::string error;
  CHECK(!b.loadState({}, error));
  CHECK(!b.loadState(std::vector<u8>(state.begin(), state.begin() + 10), error));
  CHECK(!b.loadState(std::vector<u8>(state.begin(), state.end() - 4), error));
  CHECK(error.find("damaged") != std::string::npos);
}

TEST(state_refuses_a_foreign_file) {
  Machine m(MachineType::Spectrum128);
  std::string error;
  const std::vector<u8> junk(64, 0xAA);
  CHECK(!m.loadState(junk, error));
  CHECK(error == "not a save state");
}

// The stream itself: what one direction writes, the other must read, including
// the awkward widths and the containers.
TEST(state_stream_round_trips_every_width) {
  rainbow::StateWriter writer;
  u8 small = 0x12;
  rainbow::i8 signedByte = -3;
  u16 medium = 0xBEEF;
  u32 wide = 0xDEADBEEF;
  u64 huge = 0x0123456789ABCDEFull;
  bool flag = true;
  float fraction = 0.5f;
  rainbow::i32 signedWord = -100000;
  std::array<u8, 3> fixed = {1, 2, 3};
  std::vector<u8> bytes = {9, 8, 7, 6};
  u8 table[4] = {4, 3, 2, 1};
  writer(small);
  writer(signedByte);
  writer(medium);
  writer(wide);
  writer(huge);
  writer(flag);
  writer(fraction);
  writer(signedWord);
  writer(fixed);
  writer(bytes);
  writer(table);

  const std::vector<u8>& encoded = writer.bytes();
  rainbow::StateReader reader(encoded.data(), encoded.size());
  u8 smallOut = 0;
  rainbow::i8 signedByteOut = 0;
  u16 mediumOut = 0;
  u32 wideOut = 0;
  u64 hugeOut = 0;
  bool flagOut = false;
  float fractionOut = 0.0f;
  rainbow::i32 signedWordOut = 0;
  std::array<u8, 3> fixedOut{};
  std::vector<u8> bytesOut;
  u8 tableOut[4] = {};
  reader(smallOut);
  reader(signedByteOut);
  reader(mediumOut);
  reader(wideOut);
  reader(hugeOut);
  reader(flagOut);
  reader(fractionOut);
  reader(signedWordOut);
  reader(fixedOut);
  reader(bytesOut);
  reader(tableOut);

  CHECK(reader.ok());
  CHECK(reader.consumedAll());
  CHECK_EQ(smallOut, small);
  CHECK_EQ(signedByteOut, signedByte);
  CHECK_EQ(mediumOut, medium);
  CHECK_EQ(wideOut, wide);
  CHECK_EQ(hugeOut, huge);
  CHECK_EQ(flagOut, flag);
  CHECK_EQ(fractionOut, fraction);
  CHECK_EQ(signedWordOut, signedWord);
  CHECK_EQ(fixedOut[2], 3);
  CHECK_EQ(bytesOut.size(), 4u);
  CHECK_EQ(bytesOut[3], 6);
  CHECK_EQ(tableOut[0], 4);
  CHECK_EQ(tableOut[3], 1);
}

TEST(state_stream_reports_a_short_read) {
  rainbow::StateWriter writer;
  u16 value = 0x1234;
  writer(value);
  std::vector<u8> encoded = writer.bytes();
  encoded.pop_back();

  rainbow::StateReader reader(encoded.data(), encoded.size());
  u16 out = 0;
  reader(out);
  CHECK(!reader.ok());
  CHECK(reader.error().find("ends in the middle") != std::string::npos);
}

// A body that cannot be read to the end must not be applied even partly: the
// state is proved on a scratch machine before anything here is touched.
TEST(state_refuses_a_body_it_does_not_understand) {
  Machine a(MachineType::Spectrum128);
  a.memWrite(0x8000, 0x42, 0);
  a.cpu().regs().a = 0x77;
  std::vector<u8> state = saveToBytes(a);
  // One extra byte in the body with a matching checksum is what a build that
  // added a field without bumping the version would write.
  state.insert(state.end() - 8, 0x00);
  const rainbow::u64 sum = rainbow::stateChecksum(state.data(), state.size() - 8);
  for (int i = 0; i < 8; ++i) {
    state[state.size() - 8 + size_t(i)] = u8(sum >> (8 * i));
  }

  Machine b(MachineType::Spectrum128);
  b.memWrite(0x8000, 0x11, 0);
  b.cpu().regs().a = 0x01;
  std::string error;
  CHECK(!b.loadState(state, error));
  CHECK(error.find("more data") != std::string::npos);
  CHECK_EQ(b.memRead(0x8000, 0), 0x11);
  CHECK_EQ(b.cpu().regs().a, 0x01);
}

// A frontend picks the machine from the header before it has a machine to load
// into, so the type has to be readable on its own.
TEST(state_header_names_the_machine_that_wrote_it) {
  Machine next(MachineType::Next);
  const std::vector<u8> state = saveToBytes(next);
  MachineType type = MachineType::Spectrum48;
  CHECK(Machine::peekStateType(state, type));
  CHECK_EQ(u8(type), u8(MachineType::Next));

  const std::vector<u8> junk(64, 0xAA);
  CHECK(!Machine::peekStateType(junk, type));
  CHECK(!Machine::peekStateType({}, type));
}

TEST(rewind_buffer_hands_states_back_newest_first) {
  RewindBuffer buffer;
  CHECK(!buffer.canRewind());
  std::vector<u8> out;
  CHECK(!buffer.pop(out));

  buffer.push({1, 2, 3});
  buffer.push({4, 5, 6});
  CHECK_EQ(buffer.count(), 2u);
  CHECK_EQ(buffer.bytes(), 6u);

  CHECK(buffer.pop(out));
  CHECK_EQ(out.size(), 3u);
  CHECK_EQ(out[0], 4);
  CHECK(buffer.pop(out));
  CHECK_EQ(out[0], 1);
  CHECK(!buffer.canRewind());
  CHECK_EQ(buffer.bytes(), 0u);
}

TEST(rewind_buffer_drops_the_oldest_state_to_stay_in_budget) {
  RewindBuffer buffer(10);
  buffer.push(std::vector<u8>(4, 0xAA));
  buffer.push(std::vector<u8>(4, 0xBB));
  CHECK_EQ(buffer.count(), 2u);
  buffer.push(std::vector<u8>(4, 0xCC));
  // Three states of four bytes do not fit in ten, so the oldest is gone.
  CHECK_EQ(buffer.count(), 2u);
  CHECK_EQ(buffer.bytes(), 8u);

  std::vector<u8> out;
  CHECK(buffer.pop(out));
  CHECK_EQ(out[0], 0xCC);
  CHECK(buffer.pop(out));
  CHECK_EQ(out[0], 0xBB);

  // A state bigger than the whole budget is refused instead of emptying it.
  buffer.push(std::vector<u8>(4, 0xDD));
  buffer.push(std::vector<u8>(32, 0xEE));
  CHECK_EQ(buffer.count(), 1u);
  CHECK_EQ(buffer.bytes(), 4u);

  buffer.clear();
  CHECK_EQ(buffer.count(), 0u);
  CHECK_EQ(buffer.bytes(), 0u);
}

// The point of the buffer is that a real machine can be wound back, so this
// test rewinds through actual states rather than synthetic bytes.
TEST(rewind_buffer_winds_a_machine_back) {
  Machine machine(MachineType::Spectrum128);
  loadProgram(machine, 0xC000, {0x3E, 0x01, 0x00, 0x3C, 0xC3, 0x00, 0xC0});
  RewindBuffer buffer;

  // The state is recorded before the frame it leads into, so the newest entry
  // in the buffer is always exactly one frame behind the machine.
  for (int frame = 0; frame < 3; ++frame) {
    std::vector<u8> state;
    machine.saveState(state);
    buffer.push(std::move(state));
    machine.runFrame();
  }
  const u64 frames = machine.frames();
  CHECK_EQ(frames, 3u);
  CHECK_EQ(buffer.count(), 3u);

  std::vector<u8> state;
  std::string error;
  CHECK(buffer.pop(state));
  CHECK(machine.loadState(state, error));
  CHECK_EQ(machine.frames(), 2u);

  CHECK(buffer.pop(state));
  CHECK(machine.loadState(state, error));
  CHECK_EQ(machine.frames(), 1u);
}

} // namespace
