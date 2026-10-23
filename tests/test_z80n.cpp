#include "test_framework.h"

#include <array>
#include <initializer_list>

#include "core/bus.h"
#include "core/machine.h"
#include "core/z80.h"

namespace {

using rainbow::u16;
using rainbow::u32;
using rainbow::u8;

// Flat memory, plus a recorder for the Next register file, so that the Z80N
// instructions can be checked without pulling in a whole machine.
class NextBus : public rainbow::Bus {
public:
  std::array<u8, 65536> mem{};
  std::array<u8, 65536> io{};
  std::array<u8, 256> nextRegs{};
  int nextRegWrites = 0;

  u8 memRead(u16 addr, u32) override { return mem[addr]; }
  void memWrite(u16 addr, u8 value, u32) override { mem[addr] = value; }
  u8 ioRead(u16 port, u32) override { return io[port]; }
  void ioWrite(u16 port, u8 value, u32) override { io[port] = value; }
  u8 contention(u16, u32) override { return 0; }
  void tick(u32) override {}
  u32 now() const override { return 0; }
  void nextRegWrite(u8 reg, u8 value) override {
    nextRegs[reg] = value;
    ++nextRegWrites;
  }
};

void load(NextBus& bus, std::initializer_list<u8> bytes, u16 addr = 0) {
  u16 a = addr;
  for (u8 b : bytes) bus.mem[a++] = b;
}

rainbow::Z80 makeCpu(NextBus& bus) {
  rainbow::Z80 cpu(bus);
  cpu.setExtendedInstructions(true);
  return cpu;
}

constexpr u8 ZF = 0x40, HF = 0x10, NF = 0x02;

} // namespace

TEST(z80n_is_off_on_classic_machines) {
  NextBus bus;
  load(bus, {0xED, 0x30}); // MUL D,E is a NOP on a plain Z80
  rainbow::Z80 cpu(bus);
  cpu.setBc(0x1234);
  cpu.setDe(0x0203);
  cpu.setHl(0xFFFF);

  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.hl(), 0xFFFF);
}

TEST(z80n_swapnib) {
  NextBus bus;
  load(bus, {0xED, 0x23});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x1F;

  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.regs().a, 0xF1);
}

TEST(z80n_mirror_a) {
  NextBus bus;
  load(bus, {0xED, 0x24});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0b1000'0001;

  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.regs().a, 0b1000'0001);

  cpu.regs().pc = 0;
  cpu.regs().a = 0b1100'0010;
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0b0100'0011);
}

TEST(z80n_test_sets_flags_without_touching_a) {
  NextBus bus;
  load(bus, {0xED, 0x27, 0x0F});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x35;

  CHECK_EQ(cpu.step(), 11);
  CHECK_EQ(cpu.regs().a, 0x35);
  CHECK((cpu.regs().f & ZF) == 0);
  CHECK((cpu.regs().f & HF) != 0); // AND/TEST always set the half-carry

  load(bus, {0xED, 0x27, 0x10}, 0x0000);
  cpu.regs().pc = 0;
  cpu.step();
  CHECK((cpu.regs().f & ZF) == 0);

  load(bus, {0xED, 0x27, 0x00}, 0x0000);
  cpu.regs().pc = 0;
  cpu.step();
  CHECK((cpu.regs().f & ZF) != 0);
}

TEST(z80n_shifts_de_by_b) {
  NextBus bus;
  auto cpu = makeCpu(bus);
  cpu.setBc(0x0400); // B = 4

  // BSLA DE,B
  load(bus, {0xED, 0x28});
  cpu.regs().pc = 0;
  cpu.setDe(0x0102);
  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.de(), 0x1020);

  // BSRL DE,B
  load(bus, {0xED, 0x2A});
  cpu.regs().pc = 0;
  cpu.setDe(0x8000);
  cpu.step();
  CHECK_EQ(cpu.de(), 0x0800);

  // BSRA DE,B keeps the sign
  load(bus, {0xED, 0x29});
  cpu.regs().pc = 0;
  cpu.setDe(0x8000);
  cpu.step();
  CHECK_EQ(cpu.de(), 0xF800);

  // BSRF DE,B shifts ones in
  load(bus, {0xED, 0x2B});
  cpu.regs().pc = 0;
  cpu.setDe(0x8000);
  cpu.step();
  CHECK_EQ(cpu.de(), 0xF800);
}

TEST(z80n_brlc_rotates_de) {
  NextBus bus;
  load(bus, {0xED, 0x2C});
  auto cpu = makeCpu(bus);
  cpu.setBc(0x0100);
  cpu.setDe(0x8001);

  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.de(), 0x0003);
}

TEST(z80n_mul_d_e) {
  NextBus bus;
  load(bus, {0xED, 0x30});
  auto cpu = makeCpu(bus);
  cpu.setDe(0x0020); // D = 0, E = 0x20
  cpu.regs().d = 0x10;
  cpu.setHl(0x1234);
  cpu.regs().f = 0xFF;

  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.de(), 0x0200); // the product lands in DE, not HL
  CHECK_EQ(cpu.hl(), 0x1234);
  CHECK_EQ(cpu.regs().f, 0xFF); // MUL leaves the flags alone
}

TEST(z80n_add_a_to_wide_registers) {
  NextBus bus;
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x11;

  load(bus, {0xED, 0x31});
  cpu.regs().pc = 0;
  cpu.setHl(0x00FF);
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x0110);

  load(bus, {0xED, 0x32});
  cpu.regs().pc = 0;
  cpu.setDe(0x1000);
  cpu.step();
  CHECK_EQ(cpu.de(), 0x1011);

  load(bus, {0xED, 0x33});
  cpu.regs().pc = 0;
  cpu.setBc(0x00F0);
  cpu.step();
  CHECK_EQ(cpu.bc(), 0x0101);
}

TEST(z80n_add_immediate_to_wide_registers) {
  NextBus bus;
  auto cpu = makeCpu(bus);

  load(bus, {0xED, 0x34, 0x34, 0x12});
  cpu.setHl(0x0001);
  CHECK_EQ(cpu.step(), 16);
  CHECK_EQ(cpu.hl(), 0x1235);

  load(bus, {0xED, 0x35, 0xFF, 0xFF});
  cpu.regs().pc = 0;
  cpu.setDe(0x0002);
  cpu.step();
  CHECK_EQ(cpu.de(), 0x0001);

  load(bus, {0xED, 0x36, 0x00, 0x01});
  cpu.regs().pc = 0;
  cpu.setBc(0xFFFF);
  cpu.step();
  CHECK_EQ(cpu.bc(), 0x00FF);
}

TEST(z80n_push_immediate_is_big_endian) {
  NextBus bus;
  load(bus, {0xED, 0x8A, 0x12, 0x34}); // PUSH $1234
  auto cpu = makeCpu(bus);
  cpu.regs().sp = 0x8000;

  CHECK_EQ(cpu.step(), 23);
  CHECK_EQ(bus.mem[0x7FFE], 0x34);
  CHECK_EQ(bus.mem[0x7FFF], 0x12);
  CHECK_EQ(cpu.regs().sp, 0x7FFE);
}

TEST(z80n_outinb_leaves_b_alone) {
  NextBus bus;
  load(bus, {0xED, 0x90});
  auto cpu = makeCpu(bus);
  cpu.setBc(0x4010); // B = 0x40, C = 0x10
  cpu.setHl(0x2000);
  bus.mem[0x2000] = 0x5A;

  CHECK_EQ(cpu.step(), 16);
  CHECK_EQ(bus.io[0x4010], 0x5A);
  CHECK_EQ(cpu.regs().b, 0x40);
  CHECK_EQ(cpu.hl(), 0x2001);
}

TEST(z80n_nextreg_writes_the_register_file) {
  NextBus bus;
  auto cpu = makeCpu(bus);

  load(bus, {0xED, 0x91, 0x07, 0x03}); // NEXTREG $07,$03
  CHECK_EQ(cpu.step(), 20);
  CHECK_EQ(bus.nextRegs[0x07], 0x03);

  load(bus, {0xED, 0x92, 0x15}); // NEXTREG $15,A
  cpu.regs().pc = 0;
  cpu.regs().a = 0x40;
  CHECK_EQ(cpu.step(), 17);
  CHECK_EQ(bus.nextRegs[0x15], 0x40);
  CHECK_EQ(bus.nextRegWrites, 2);
}

TEST(z80n_pixelad_and_pixeldn) {
  NextBus bus;
  auto cpu = makeCpu(bus);

  load(bus, {0xED, 0x94}); // PIXELAD, D = Y, E = X
  cpu.regs().d = 0;
  cpu.regs().e = 0;
  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.hl(), 0x4000);

  load(bus, {0xED, 0x94}, 0x0000);
  cpu.regs().pc = 0;
  cpu.regs().d = 1; // second pixel row starts 0x0100 further down
  cpu.regs().e = 0;
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x4100);

  load(bus, {0xED, 0x94}, 0x0000);
  cpu.regs().pc = 0;
  cpu.regs().d = 0;
  cpu.regs().e = 0x40; // byte 8 of the line
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x4008);
}

TEST(z80n_pixeldn_walks_the_bitmap_rows) {
  NextBus bus;
  load(bus, {0xED, 0x93});
  auto cpu = makeCpu(bus);

  cpu.setHl(0x4000);
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x4100);

  // H = 0x47 is the last row of the character cell, so the next pixel row
  // wraps back to the top third and moves 32 bytes to the right instead.
  cpu.regs().pc = 0;
  cpu.setHl(0x4700);
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x4020);

  cpu.regs().pc = 0;
  cpu.setHl(0x47E0);
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x4800);
}

TEST(z80n_setae) {
  NextBus bus;
  load(bus, {0xED, 0x95});
  auto cpu = makeCpu(bus);

  cpu.regs().e = 0;
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x80);

  cpu.regs().pc = 0;
  cpu.regs().e = 7;
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x01);
}

TEST(z80n_jp_c) {
  NextBus bus;
  load(bus, {0xED, 0x98}, 0x1200);
  auto cpu = makeCpu(bus);
  cpu.regs().pc = 0x1200;
  cpu.setBc(0x0055);
  bus.io[0x0055] = 0x40;

  CHECK_EQ(cpu.step(), 13);
  CHECK_EQ(cpu.regs().pc, 0x1240);
}

TEST(z80n_ldix_copies_one_byte) {
  NextBus bus;
  load(bus, {0xED, 0xA4});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x00;
  cpu.setHl(0x1000);
  cpu.setDe(0x2000);
  cpu.setBc(0x0002);
  cpu.regs().f = 0xFF;
  bus.mem[0x1000] = 0x11;

  CHECK_EQ(cpu.step(), 16);
  CHECK_EQ(bus.mem[0x2000], 0x11);
  CHECK_EQ(cpu.hl(), 0x1001);
  CHECK_EQ(cpu.de(), 0x2001);
  CHECK_EQ(cpu.bc(), 0x0001);
  CHECK_EQ(cpu.regs().f, 0xFF); // no flag changes
}

TEST(z80n_ldix_skips_a_byte_equal_to_a) {
  NextBus bus;
  load(bus, {0xED, 0xA4});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x42;
  cpu.setHl(0x1000);
  cpu.setDe(0x2000);
  cpu.setBc(0x0001);
  bus.mem[0x1000] = 0x42;
  bus.mem[0x2000] = 0xEE;

  cpu.step();
  CHECK_EQ(bus.mem[0x2000], 0xEE);
  CHECK_EQ(cpu.de(), 0x2001);
}

TEST(z80n_lddx_decrements_hl) {
  NextBus bus;
  load(bus, {0xED, 0xAC});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x00;
  cpu.setHl(0x1002);
  cpu.setDe(0x2000);
  cpu.setBc(0x0003);
  bus.mem[0x1002] = 0x77;

  CHECK_EQ(cpu.step(), 16);
  CHECK_EQ(bus.mem[0x2000], 0x77);
  CHECK_EQ(cpu.hl(), 0x1001);
  CHECK_EQ(cpu.de(), 0x2001);
}

TEST(z80n_ldirx_repeats_until_bc_is_zero) {
  NextBus bus;
  load(bus, {0xED, 0xB4});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x00;
  cpu.setHl(0x1000);
  cpu.setDe(0x2000);
  cpu.setBc(0x0003);
  bus.mem[0x1000] = 0x01;
  bus.mem[0x1001] = 0x02;
  bus.mem[0x1002] = 0x03;

  CHECK_EQ(cpu.step(), 21); // the repeat iteration costs the extra 5
  CHECK_EQ(cpu.regs().pc, 0);
  CHECK_EQ(cpu.bc(), 0x0002);

  cpu.step();
  cpu.step();
  CHECK_EQ(bus.mem[0x2000], 0x01);
  CHECK_EQ(bus.mem[0x2001], 0x02);
  CHECK_EQ(bus.mem[0x2002], 0x03);
  CHECK_EQ(cpu.bc(), 0);
  CHECK_EQ(cpu.regs().pc, 2); // fell through once the block was done
}

TEST(z80n_ldirx_stops_on_a_matching_byte) {
  NextBus bus;
  load(bus, {0xED, 0xB4});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x00;
  cpu.setHl(0x1000);
  cpu.setDe(0x2000);
  cpu.setBc(0x0004);
  bus.mem[0x1000] = 0x09;
  bus.mem[0x1001] = 0x00; // terminator

  cpu.step();
  CHECK_EQ(cpu.bc(), 0x0003);
  cpu.step();
  CHECK_EQ(cpu.bc(), 0x0002);
  CHECK_EQ(cpu.regs().pc, 2); // the match ended the loop
}

TEST(z80n_ldws_copies_vertically) {
  NextBus bus;
  load(bus, {0xED, 0xA5});
  auto cpu = makeCpu(bus);
  cpu.setHl(0x1000);
  cpu.setDe(0x20F0);
  bus.mem[0x1000] = 0x33;

  CHECK_EQ(cpu.step(), 14);
  CHECK_EQ(bus.mem[0x20F0], 0x33);
  CHECK_EQ(cpu.regs().l, 0x01);
  CHECK_EQ(cpu.regs().d, 0x21);
  CHECK((cpu.regs().f & NF) == 0);
  CHECK((cpu.regs().f & HF) == 0); // D went 0x20 -> 0x21, low nibble changed
  CHECK((cpu.regs().f & ZF) == 0);
}

TEST(z80n_ldpirx_reads_from_an_aligned_pattern) {
  NextBus bus;
  load(bus, {0xED, 0xB7});
  auto cpu = makeCpu(bus);
  cpu.regs().a = 0x00;
  cpu.setHl(0x1003); // masked down to 0x1000
  cpu.setDe(0x2000);
  cpu.setBc(0x0002);
  cpu.regs().e = 0x05; // picks the sixth byte of the pattern
  bus.mem[0x1005] = 0xAA;

  CHECK_EQ(cpu.step(), 21);
  CHECK_EQ(bus.mem[0x2005], 0xAA); // DE carries the offset too, as on hardware
  CHECK_EQ(cpu.de(), 0x2006); // DE still counts up
  CHECK_EQ(cpu.hl(), 0x1003); // HL is left alone
  CHECK_EQ(cpu.bc(), 0x0001);
}

TEST(z80n_is_enabled_only_on_the_next) {
  rainbow::Machine next(rainbow::MachineType::Next);
  rainbow::Machine plus3(rainbow::MachineType::Plus3);
  rainbow::Machine classic(rainbow::MachineType::Spectrum48);

  CHECK(next.cpu().extendedInstructions());
  CHECK(!plus3.cpu().extendedInstructions());
  CHECK(!classic.cpu().extendedInstructions());
}
