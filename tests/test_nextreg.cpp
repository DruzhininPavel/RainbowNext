#include "test_framework.h"

#include "core/machine.h"
#include "core/nextreg.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::NextRegFile;
using mynext::u16;
using mynext::u32;
using mynext::u8;
using mynext::tstatesPerLine;

namespace {

// The ROM is all zeros unless it is loaded, which makes "is this the ROM or
// RAM?" invisible. A distinct pattern per byte makes both halves testable.
void loadPatternRom(Machine& m) {
  u8 bank[0x4000];
  for (size_t i = 0; i < sizeof bank; ++i) bank[i] = u8(i * 7 + 3);
  m.loadRomData(bank, sizeof bank, 0);
}

} // namespace

TEST(nextreg_mmu_defaults_match_the_soft_reset_state) {
  Machine m(MachineType::Next);
  const NextRegFile& r = m.nextRegs();

  CHECK_EQ(r.mmuSlot(0), 0xFF);
  CHECK_EQ(r.mmuSlot(1), 0xFF);
  CHECK_EQ(r.mmuSlot(2), 0x0A);
  CHECK_EQ(r.mmuSlot(3), 0x0B);
  CHECK_EQ(r.mmuSlot(4), 0x04);
  CHECK_EQ(r.mmuSlot(5), 0x05);
  CHECK_EQ(r.mmuSlot(6), 0x00);
  CHECK_EQ(r.mmuSlot(7), 0x01);
  CHECK_EQ(r.turbo(), 0);
}

TEST(nextreg_read_only_registers_keep_their_values) {
  Machine m(MachineType::Next);

  CHECK_EQ(m.readNextReg(0x00), 0x08); // an emulator
  CHECK_EQ(m.readNextReg(0x01), 0x30); // core 3.0

  m.writeNextReg(0x00, 0xAA);
  m.writeNextReg(0x01, 0xBB);
  m.writeNextReg(0x1E, 0xCC);
  CHECK_EQ(m.readNextReg(0x00), 0x08);
  CHECK_EQ(m.readNextReg(0x01), 0x30);
  CHECK_EQ(m.readNextReg(0x1E), 0x00);
}

TEST(nextreg_ports_select_and_expose_a_register) {
  Machine m(MachineType::Next);

  m.ioWrite(0x243B, 0x07, 0);
  CHECK_EQ(m.nextRegs().selected(), 0x07);
  CHECK_EQ(m.ioRead(0x243B, 0), 0x07);

  m.ioWrite(0x253B, 0x02, 0); // turbo 14 MHz
  CHECK_EQ(m.ioRead(0x253B, 0), 0x02);
  CHECK_EQ(m.nextRegs().turbo(), 2);

  // Reading the selector port must not disturb the selected register.
  CHECK_EQ(m.ioRead(0x243B, 0), 0x07);
}

TEST(nextreg_mmu_page_255_shows_the_rom) {
  Machine m(MachineType::Next);
  loadPatternRom(m);

  CHECK_EQ(m.memRead(0x0000, 0), 0x03);
  CHECK_EQ(m.memRead(0x0001, 0), 0x0A);

  m.writeNextReg(0x50, 0x03); // RAM page 3 at 0x0000
  CHECK_EQ(m.memRead(0x0000, 0), 0x00);
  m.memWrite(0x0000, 0x77, 0);
  CHECK_EQ(m.memRead(0x0000, 0), 0x77);

  m.writeNextReg(0x50, NextRegFile::RomPage);
  CHECK_EQ(m.memRead(0x0000, 0), 0x03);
  CHECK_EQ(m.memRead(0x0001, 0), 0x0A);
}

TEST(nextreg_mmu_slots_are_8k_wide) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x50, 0x04);
  m.writeNextReg(0x51, 0x05);
  m.memWrite(0x0000, 0x11, 0);
  m.memWrite(0x1FFF, 0x22, 0);
  m.memWrite(0x2000, 0x33, 0);

  // Swapping the two slots must swap the two 8K halves, not the whole 16K.
  m.writeNextReg(0x50, 0x05);
  m.writeNextReg(0x51, 0x04);
  CHECK_EQ(m.memRead(0x0000, 0), 0x33);
  CHECK_EQ(m.memRead(0x1FFF, 0), 0x00); // page 5 was never written
  CHECK_EQ(m.memRead(0x2000, 0), 0x11);
  CHECK_EQ(m.memRead(0x3FFF, 0), 0x22);
}

TEST(nextreg_nextreg_instruction_repages_memory) {
  Machine m(MachineType::Next);
  loadPatternRom(m);

  m.writeNextReg(0x50, 0x00); // run from RAM page 0
  // ED 91 50 05 = NEXTREG 0x50, 0x05
  m.memWrite(0x0000, 0xED, 0);
  m.memWrite(0x0001, 0x91, 0);
  m.memWrite(0x0002, 0x50, 0);
  m.memWrite(0x0003, 0x05, 0);

  m.cpu().regs().pc = 0x0000;
  m.stepInstruction();

  CHECK_EQ(m.nextRegs().mmuSlot(0), 0x05);
  CHECK_EQ(m.memRead(0x0000, 0), 0x00); // page 5 is untouched RAM
  m.writeNextReg(0x50, 0x00);
  CHECK_EQ(m.memRead(0x0000, 0), 0xED);
}

TEST(nextreg_port_7ffd_repages_the_high_slots) {
  Machine m(MachineType::Next);

  m.ioWrite(0x7FFD, 0x03, 0); // bank 3 at 0xC000
  CHECK_EQ(m.nextRegs().mmuSlot(6), 0x06);
  CHECK_EQ(m.nextRegs().mmuSlot(7), 0x07);

  m.memWrite(0xC000, 0x5A, 0); // page 6, offset 0
  m.writeNextReg(0x57, 0x06);  // page 6 at 0xE000 instead
  CHECK_EQ(m.memRead(0xE000, 0), 0x5A);

  // Locking 0x7FFD must freeze the classic port as well.
  m.ioWrite(0x7FFD, 0x20, 0);
  CHECK(m.pagingLocked());
  m.ioWrite(0x7FFD, 0x01, 0);
  CHECK_EQ(m.nextRegs().mmuSlot(6), 0x06);
  CHECK_EQ(m.nextRegs().mmuSlot(7), 0x06);
}

TEST(nextreg_reset_register_clears_ram) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x50, 0x02);
  m.memWrite(0x0000, 0x99, 0);
  m.writeNextReg(0x02, 0x01); // soft reset

  CHECK_EQ(m.nextRegs().mmuSlot(0), NextRegFile::RomPage);
  m.writeNextReg(0x50, 0x02);
  CHECK_EQ(m.memRead(0x0000, 0), 0x00);
}

TEST(nextreg_turbo_field_is_stored) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x07, 0x03);
  CHECK_EQ(m.nextRegs().turbo(), 3);
  m.writeNextReg(0x07, 0xFD); // the upper bits are not part of the field
  CHECK_EQ(m.nextRegs().turbo(), 1);
}

TEST(nextreg_raster_counter_reports_the_current_line) {
  Machine m(MachineType::Next);
  const u32 line = tstatesPerLine(m.timings());

  m.runCycles(line * 10 + 5);
  CHECK_EQ(m.readNextReg(0x1E), 10);
  CHECK_EQ(m.readNextReg(0x1F), 0);
}

TEST(nextreg_classic_machines_have_no_next_registers) {
  Machine m(MachineType::Spectrum128);

  m.ioWrite(0x243B, 0x50, 0);
  CHECK_EQ(m.nextRegs().selected(), 0x00);
  CHECK_EQ(m.ioRead(0x243B, 0), 0xFF);
  CHECK_EQ(m.ioRead(0x253B, 0), 0xFF);
}
