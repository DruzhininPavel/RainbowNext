#include <cstdio>
#include <string>
#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/nextreg.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::NextRegFile;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;
using rainbow::tstatesPerLine;

namespace {

// The ROM is all zeros unless it is loaded, which makes "is this the ROM or
// RAM?" invisible. A distinct pattern per byte makes both halves testable.
std::vector<u8> readFile(const std::string& path) {
  std::vector<u8> data;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return data;
  u8 buffer[0x4000];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
    data.insert(data.end(), buffer, buffer + got);
  }
  std::fclose(file);
  return data;
}

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

  CHECK_EQ(m.readNextReg(0x00), 0x0A); // a ZX Spectrum Next
  CHECK_EQ(m.readNextReg(0x01), 0x31); // core 3.01

  m.writeNextReg(0x00, 0xAA);
  m.writeNextReg(0x01, 0xBB);
  m.writeNextReg(0x1E, 0xCC);
  CHECK_EQ(m.readNextReg(0x00), 0x0A);
  CHECK_EQ(m.readNextReg(0x01), 0x31);
  CHECK_EQ(m.readNextReg(0x1E), 0x00);
}

TEST(nextreg_02_raises_an_nmi_without_resetting) {
  Machine m(MachineType::Next);
  m.cpu().regs().pc = 0x1234;
  m.cpu().regs().sp = 0x8000;

  // Bit 2 is the divMMC NMI and bit 3 the multiface one; neither is a reset.
  m.writeNextReg(0x02, 0x04);
  CHECK_EQ(m.cpu().regs().pc, 0x0066);
  CHECK(!m.cpu().regs().iff1);
  CHECK_EQ(m.cpu().regs().sp, u16(0x7FFE));
  CHECK_EQ(m.memRead(u16(0x7FFE), 0), u8(0x34));
  CHECK_EQ(m.memRead(u16(0x7FFF), 0), u8(0x12));

  m.cpu().regs().pc = 0x4321;
  m.writeNextReg(0x02, 0x08);
  CHECK_EQ(m.cpu().regs().pc, 0x0066);
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

TEST(nextreg_soft_reset_keeps_ram_and_hard_reset_clears_it) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x50, 0x02);
  m.memWrite(0x0000, 0x99, 0);
  m.writeNextReg(0x02, 0x01); // soft reset

  CHECK_EQ(m.nextRegs().mmuSlot(0), NextRegFile::RomPage);
  CHECK_EQ(m.readNextReg(0x02), 0x01); // the ROM can tell it was a warm start
  m.writeNextReg(0x50, 0x02);
  CHECK_EQ(m.memRead(0x0000, 0), 0x99);

  m.writeNextReg(0x02, 0x02); // hard reset

  CHECK_EQ(m.readNextReg(0x02), 0x02);
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

TEST(nextreg_alternate_rom_swaps_the_low_rom_bank) {
  Machine m(MachineType::Next);
  loadPatternRom(m); // bank 0

  CHECK_EQ(m.memRead(0x0000, 0), 0x03);
  CHECK_EQ(m.memRead(0x0001, 0), 0x0A);

  // Bit 6 makes the window writable, which is how the boot ROM fills the alt
  // banks before switching to them with bit 7.
  m.writeNextReg(0x8C, 0xC0);
  m.memWrite(0x0000, 0x55, 0);
  m.memWrite(0x0001, 0xAA, 0);
  m.memWrite(0x2000, 0x11, 0); // the upper half of the same 16K alt bank

  m.writeNextReg(0x8C, 0x80);
  CHECK_EQ(m.memRead(0x0000, 0), 0x55);
  CHECK_EQ(m.memRead(0x0001, 0), 0xAA);
  CHECK_EQ(m.memRead(0x2000, 0), 0x11);

  m.writeNextReg(0x8C, 0x00);
  CHECK_EQ(m.memRead(0x0000, 0), 0x03);
  CHECK_EQ(m.memRead(0x0001, 0), 0x0A);
}

// Register 0x8E exposes the whole 128K memory map at once, which is what the
// Next firmware uses instead of poking 0x7FFD and 0x1FFD one bit at a time.
TEST(nextreg_0x8e_maps_the_128k_memory_in_one_write) {
  Machine m(MachineType::Next);
  loadPatternRom(m); // bank 0

  // Bits 0 and 1 select the ROM bank and are readable through the classic
  // ports, so the register and 0x7FFD/0x1FFD have to agree in both directions.
  m.writeNextReg(0x8E, 0x03); // ROM 3, bit 3 clear: the RAM map is untouched
  CHECK_EQ(m.port7ffd() & 0x10, 0x10);
  CHECK_EQ(m.port1ffd() & 0x04, 0x04);
  CHECK_EQ(m.readNextReg(0x8E) & 0x07, 0x03);

  m.writeNextReg(0x8E, 0x00); // back to ROM 0
  CHECK_EQ(m.port7ffd() & 0x10, 0x00);
  CHECK_EQ(m.port1ffd() & 0x04, 0x00);

  // Bit 3 asks for the RAM bank in bits 6:4; without it the register leaves
  // the mapping alone, which is how a caller changes only the ROM.
  m.writeNextReg(0x8E, 0x3A); // bank 3, ROM 2, paging mode bit set
  CHECK_EQ(m.nextRegs().mmuSlot(6), 0x06);
  CHECK_EQ(m.nextRegs().mmuSlot(7), 0x07);
  CHECK_EQ(m.readNextReg(0x8E), 0x3A);

  m.writeNextReg(0x8E, 0x30); // bank 3 again, RAM bits already set
  CHECK_EQ(m.nextRegs().mmuSlot(6), 0x06);

  // Bit 2 is the +2A/+3 paging mode bit of port 0x1FFD.
  m.writeNextReg(0x8E, 0x3C);
  CHECK_EQ(m.port1ffd() & 0x01, 0x01);
  CHECK_EQ(m.readNextReg(0x8E) & 0x04, 0x04);
}

TEST(next_ula_screen_bank_follows_port_7ffd) {
  Machine m(MachineType::Next);
  loadPatternRom(m);

  // The Next maps memory through the MMU registers, but the ULA still takes
  // its screen from bit 3 of 0x7FFD. A game that keeps Layer 2 in the low
  // banks uses it to pick the free one, so it has to move the ULA with it.
  m.ioWrite(0x7FFD, 0x00, 0);
  CHECK_EQ(m.screenBank(), 5);
  CHECK_EQ(m.ramBank(), 0);

  m.ioWrite(0x7FFD, 0x08, 0);
  CHECK_EQ(m.screenBank(), 7);

  m.ioWrite(0x7FFD, 0x0B, 0);
  CHECK_EQ(m.screenBank(), 7);
  CHECK_EQ(m.ramBank(), 3);
}

// Register 0x8E exposes the whole 128K memory map at once, which is what the
// Next firmware uses instead of poking 0x7FFD and 0x1FFD one bit at a time.
TEST(nextreg_machine_type_needs_the_unlock_bit) {
  Machine m(MachineType::Next);

  CHECK_EQ(m.readNextReg(0x03), 0x33); // +2A/+3 timing, +2A/+3 type
  m.writeNextReg(0x03, 0x60);
  CHECK_EQ(m.readNextReg(0x03), 0x33);

  m.writeNextReg(0x03, 0xE0); // unlock, then ask for 110 timing
  CHECK_EQ(m.readNextReg(0x03), 0x63);
}

TEST(nextreg_turbo_gives_the_cpu_more_work_per_frame) {
  // LD HL,0 / INC HL / JR -3, an 18 T-state loop whose count is readable
  // straight from HL when the frame ends.
  auto runOneFrame = [](u8 speed) {
    Machine m(MachineType::Next);
    m.writeNextReg(0x07, speed);
    m.writeNextReg(0x50, 0x00);
    m.writeNextReg(0x51, 0x01);
    m.memWrite(0x0000, 0x21, 0);
    m.memWrite(0x0001, 0x00, 0);
    m.memWrite(0x0002, 0x00, 0);
    m.memWrite(0x0003, 0x23, 0);
    m.memWrite(0x0004, 0x18, 0);
    m.memWrite(0x0005, 0xFD, 0);
    m.cpu().regs().pc = 0x0000;
    m.runFrame();
    CHECK_EQ(m.frames(), 1); // the ULA still shows exactly one frame
    return u32(m.cpu().hl());
  };

  const u32 base = runOneFrame(0);
  CHECK(base > 3000);
  const u32 fast = runOneFrame(1);
  CHECK(fast > base * 19 / 10);
  CHECK(fast < base * 21 / 10);
  const u32 fastest = runOneFrame(3);
  CHECK(fastest > base * 7);
}

// The Next keeps the 128K ROM in the bottom slots, so the 128K menu is the
// quickest proof that the MMU, the ROM paging and the 0x7FFD shortcut agree.
TEST(next_boots_the_128k_rom_through_the_mmu) {
  Machine m(MachineType::Next);
  std::vector<u8> image;
  for (const char* name : {"128-0.rom", "128-1.rom"}) {
    const std::vector<u8> bank = readFile(std::string(RAINBOW_SOURCE_DIR) + "/roms/" + name);
    if (bank.size() != 0x4000) return; // no ROM set available
    image.insert(image.end(), bank.begin(), bank.end());
  }
  if (!m.loadRomImageData(image.data(), image.size())) return;

  for (int i = 0; i < 300; ++i) m.runFrame();

  int lit = 0;
  for (u32 addr = 0x4000; addr < 0x5B00; ++addr) {
    if (m.memRead(u16(addr), 0) != 0) ++lit;
  }
  CHECK(lit > 100); // the menu has been drawn
}

// NR 0x08 is the machine-type register. It is readable, so a loader can read
// it back, edit one bit and write it again, which is exactly what nexload does
// before it hands control to a .nex program.
TEST(nextreg_08_reads_back_the_value_that_was_written) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x08, 0xCE);
  CHECK_EQ(m.readNextReg(0x08), u8(0xCE));

  // A read-modify-write that only touches one bit must keep the others.
  const u8 value = u8((m.readNextReg(0x08) & 0x10u) | 0xCEu);
  m.writeNextReg(0x08, value);
  CHECK_EQ(m.readNextReg(0x08), u8(0xCE));
}
