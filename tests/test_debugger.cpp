#include "test_framework.h"

#include "core/machine.h"

namespace {

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u16;
using rainbow::u8;

// A 128K machine with a hand-written program in RAM, so the tests never depend
// on a ROM image being present.
void loadProgram(Machine& machine, u16 address, std::initializer_list<u8> code) {
  u16 at = address;
  for (u8 byte : code) machine.memWrite(at++, byte, 0);
  machine.cpu().regs().pc = address;
}

} // namespace

TEST(debugger_breakpoint_list_stays_sorted_and_unique) {
  Machine m(MachineType::Spectrum128);
  CHECK(m.breakpoints().empty());
  CHECK(!m.hasBreakpoint(0x8000));

  m.setBreakpoint(0x9000);
  m.setBreakpoint(0x8000);
  m.setBreakpoint(0x9000);
  CHECK_EQ(m.breakpoints().size(), 2);
  CHECK_EQ(m.breakpoints()[0], 0x8000);
  CHECK_EQ(m.breakpoints()[1], 0x9000);
  CHECK(m.hasBreakpoint(0x8000));
  CHECK(!m.hasBreakpoint(0x8001));

  m.clearBreakpoint(0x8000);
  CHECK(!m.hasBreakpoint(0x8000));
  CHECK_EQ(m.breakpoints().size(), 1);
  // Clearing something that was never set is a no-op.
  m.clearBreakpoint(0x1234);
  CHECK_EQ(m.breakpoints().size(), 1);

  m.clearBreakpoints();
  CHECK(m.breakpoints().empty());
}

TEST(debugger_run_frame_stops_before_the_breakpoint) {
  Machine m(MachineType::Spectrum128);
  // LD A,1 / JP 0xC000, so the frame would otherwise never leave this loop.
  loadProgram(m, 0xC000, {0x3E, 0x01, 0xC3, 0x00, 0xC0});
  m.setBreakpoint(0xC002);

  const auto stopped = m.runFrame();
  CHECK(stopped.has_value());
  CHECK_EQ(*stopped, 0xC002);
  CHECK_EQ(m.cpu().regs().pc, 0xC002);
  // Stopping early must not finish the frame, or the caller would think a
  // whole frame of emulation happened.
  CHECK_EQ(m.frames(), 0u);
  // The instruction at the breakpoint has not run yet.
  CHECK_EQ(m.cpu().regs().a, 0x01);
}

TEST(debugger_run_frame_completes_without_breakpoints) {
  Machine m(MachineType::Spectrum128);
  loadProgram(m, 0xC000, {0x3E, 0x01, 0xC3, 0x00, 0xC0});

  CHECK(!m.runFrame().has_value());
  CHECK_EQ(m.frames(), 1u);
}

TEST(debugger_profiler_counts_instruction_starts) {
  Machine m(MachineType::Spectrum128);
  loadProgram(m, 0xC000, {0x00, 0xC3, 0x00, 0xC0}); // NOP / JP 0xC000

  // Profiling is off until a frontend asks for it.
  m.stepInstruction();
  m.stepInstruction();
  CHECK_EQ(m.sampleCount(0xC000), 0u);
  CHECK_EQ(m.sampleTotal(), 0u);

  m.setProfiling(true);
  CHECK(m.profiling());
  CHECK_EQ(m.sampleCount(0xC000), 0u);

  for (int i = 0; i < 5; ++i) m.stepInstruction();
  // Five instructions from 0xC000 alternate NOP and JP, so the NOP address
  // wins by one and every instruction was counted exactly once.
  CHECK_EQ(m.sampleCount(0xC000), 3u);
  CHECK_EQ(m.sampleCount(0xC001), 2u);
  CHECK_EQ(m.sampleTotal(), 5u);

  m.resetProfile();
  CHECK_EQ(m.sampleCount(0xC000), 0u);
  CHECK_EQ(m.sampleTotal(), 0u);

  m.setProfiling(false);
  m.stepInstruction();
  CHECK_EQ(m.sampleTotal(), 0u);
  CHECK(!m.profiling());
}

TEST(debugger_profiler_attributes_counts_per_address) {
  Machine m(MachineType::Spectrum128);
  // LD A,1 / NOP / JP 0xC000, so the loop is three instructions long and the
  // immediate of the LD is never the start of one.
  loadProgram(m, 0xC000, {0x3E, 0x01, 0x00, 0xC3, 0x00, 0xC0});
  m.setProfiling(true);
  for (int i = 0; i < 6; ++i) m.stepInstruction();

  CHECK_EQ(m.sampleCount(0xC000), 2u); // the LD at the top of the loop
  CHECK_EQ(m.sampleCount(0xC002), 2u); // the NOP
  CHECK_EQ(m.sampleCount(0xC003), 2u); // the JP back
  CHECK_EQ(m.sampleCount(0xC001), 0u); // an operand byte, never an instruction
  CHECK_EQ(m.sampleTotal(), 6u);
}
