#include "test_framework.h"

#include <array>
#include <initializer_list>

#include "core/bus.h"
#include "core/z80.h"

namespace {

using rainbow::u16;
using rainbow::u32;
using rainbow::u8;

// A bus with flat 64K memory and no contention, so that the Z80 core can be
// exercised independently of the ULA.
class FlatBus : public rainbow::Bus {
public:
  std::array<u8, 65536> mem{};
  std::array<u8, 65536> io{};
  u16 lastOutPort = 0;
  u8 lastOutValue = 0;

  u8 memRead(u16 addr, u32) override { return mem[addr]; }
  void memWrite(u16 addr, u8 value, u32) override { mem[addr] = value; }
  u8 ioRead(u16 port, u32) override { return io[port]; }
  void ioWrite(u16 port, u8 value, u32) override {
    lastOutPort = port;
    lastOutValue = value;
  }
  u8 contention(u16, u32) override { return 0; }
  void tick(u32) override {}
  u32 now() const override { return 0; }
};

void load(FlatBus& bus, std::initializer_list<u8> bytes, u16 addr = 0) {
  u16 a = addr;
  for (u8 b : bytes) bus.mem[a++] = b;
}

constexpr u8 SF = 0x80, ZF = 0x40, HF = 0x10;
constexpr u8 PF = 0x04, NF = 0x02, CF = 0x01;

} // namespace

TEST(z80_nop_timing) {
  FlatBus bus;
  load(bus, {0x00});
  rainbow::Z80 cpu(bus);
  CHECK_EQ(cpu.step(), 4);
  CHECK_EQ(cpu.regs().pc, 1);
}

TEST(z80_load_immediates) {
  FlatBus bus;
  load(bus, {0x3E, 0x42, 0x01, 0x34, 0x12});
  rainbow::Z80 cpu(bus);

  CHECK_EQ(cpu.step(), 7);
  CHECK_EQ(cpu.regs().a, 0x42);

  CHECK_EQ(cpu.step(), 10);
  CHECK_EQ(cpu.bc(), 0x1234);
}

TEST(z80_store_immediate_through_hl) {
  FlatBus bus;
  load(bus, {0x21, 0x00, 0x80, 0x36, 0x55}); // LD HL,8000h : LD (HL),55h
  rainbow::Z80 cpu(bus);
  cpu.step();
  CHECK_EQ(cpu.step(), 10);
  CHECK_EQ(bus.mem[0x8000], 0x55);
}

TEST(z80_inc_dec_flags) {
  FlatBus bus;
  load(bus, {0x3E, 0x7F, 0x3C, 0x3D}); // LD A,7Fh : INC A : DEC A
  rainbow::Z80 cpu(bus);
  cpu.regs().f = 0;
  cpu.step();

  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x80);
  CHECK(cpu.regs().f & SF);
  CHECK(cpu.regs().f & HF);
  CHECK(cpu.regs().f & PF);
  CHECK(!(cpu.regs().f & NF));
  CHECK(!(cpu.regs().f & CF));

  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x7F);
  CHECK(!(cpu.regs().f & SF));
  CHECK(cpu.regs().f & HF);
  CHECK(cpu.regs().f & PF);
  CHECK(cpu.regs().f & NF);
}

TEST(z80_add_carry_flags) {
  FlatBus bus;
  load(bus, {0x3E, 0xFF, 0xC6, 0x01}); // LD A,FFh : ADD A,01h
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x00);
  CHECK(cpu.regs().f & ZF);
  CHECK(cpu.regs().f & CF);
  CHECK(cpu.regs().f & HF);
  CHECK(!(cpu.regs().f & NF));
}

TEST(z80_daa_after_add) {
  FlatBus bus;
  load(bus, {0x3E, 0x15, 0xC6, 0x27, 0x27}); // LD A,15h : ADD A,27h : DAA
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x3C);
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x42);
  CHECK(!(cpu.regs().f & CF));
}

TEST(z80_daa_after_sub) {
  FlatBus bus;
  load(bus, {0x3E, 0x42, 0xD6, 0x27, 0x27}); // LD A,42h : SUB 27h : DAA
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x1B);
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x15);
}

TEST(z80_cb_rotate) {
  FlatBus bus;
  load(bus, {0x06, 0x81, 0xCB, 0x00}); // LD B,81h : RLC B
  rainbow::Z80 cpu(bus);
  cpu.step();
  CHECK_EQ(cpu.step(), 8);
  CHECK_EQ(cpu.regs().b, 0x03);
  CHECK(cpu.regs().f & CF);
}

TEST(z80_cb_sll_undocumented) {
  FlatBus bus;
  load(bus, {0x06, 0x81, 0xCB, 0x30}); // LD B,81h : SLL B
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().b, 0x03);
  CHECK(cpu.regs().f & CF);
}

TEST(z80_cb_bit) {
  FlatBus bus;
  load(bus, {0x06, 0x04, 0xCB, 0x58}); // LD B,04h : BIT 3,B
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK(cpu.regs().f & ZF);
  CHECK(cpu.regs().f & HF);
}

TEST(z80_ed_neg) {
  FlatBus bus;
  load(bus, {0x3E, 0x01, 0xED, 0x44}); // LD A,01h : NEG
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0xFF);
  CHECK(cpu.regs().f & CF);
  CHECK(cpu.regs().f & NF);
}

TEST(z80_ed_adc_hl) {
  FlatBus bus;
  load(bus, {0x21, 0xFF, 0x0F, 0x01, 0x01, 0x00, 0xED, 0x4A});
  rainbow::Z80 cpu(bus); // LD HL,0FFFh : LD BC,0001h : ADC HL,BC
  cpu.regs().f = 0;
  cpu.step();
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.hl(), 0x1000);
  CHECK(!(cpu.regs().f & CF));
  CHECK(cpu.regs().f & HF);
}

TEST(z80_ed_ldir) {
  FlatBus bus;
  bus.mem[0x8000] = 0x11;
  bus.mem[0x8001] = 0x22;
  bus.mem[0x8002] = 0x33;
  load(bus, {0x21, 0x00, 0x80, 0x11, 0x00, 0x90, 0x01, 0x03, 0x00, 0xED, 0xB0});
  rainbow::Z80 cpu(bus);
  cpu.step(); // LD HL,8000h
  cpu.step(); // LD DE,9000h
  cpu.step(); // LD BC,0003h

  for (int i = 0; i < 3; ++i) cpu.step();

  CHECK_EQ(cpu.bc(), 0);
  CHECK_EQ(cpu.hl(), 0x8003);
  CHECK_EQ(cpu.de(), 0x9003);
  CHECK_EQ(bus.mem[0x9000], 0x11);
  CHECK_EQ(bus.mem[0x9001], 0x22);
  CHECK_EQ(bus.mem[0x9002], 0x33);
}

TEST(z80_ed_cpir) {
  FlatBus bus;
  bus.mem[0x8000] = 0x00;
  bus.mem[0x8001] = 0x00;
  bus.mem[0x8002] = 0x99;
  load(bus, {0x21, 0x00, 0x80, 0x01, 0x08, 0x00, 0x3E, 0x99, 0xED, 0xB1});
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  cpu.step();
  cpu.regs().f = 0;
  for (int i = 0; i < 8 && cpu.regs().pc == 0x0008; ++i) cpu.step(); // CPIR repeats

  CHECK(cpu.regs().f & ZF);
  CHECK_EQ(cpu.hl(), 0x8003);
  CHECK_EQ(cpu.bc(), 5);
}

TEST(z80_dd_ix_offsets) {
  FlatBus bus;
  bus.mem[0x8005] = 0x42;
  load(bus, {0xDD, 0x21, 0x00, 0x80, 0xDD, 0x7E, 0x05}); // LD IX,8000h : LD A,(IX+5)
  rainbow::Z80 cpu(bus);
  cpu.step();
  CHECK_EQ(cpu.regs().ix, 0x8000);
  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x42);
}

TEST(z80_dd_ld_ixh_imm) {
  FlatBus bus;
  load(bus, {0xDD, 0x26, 0x42, 0xDD, 0x2E, 0x17}); // LD IXH,42h : LD IXL,17h
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().ix, 0x4217);
}

TEST(z80_ddcb_rlc_memory) {
  FlatBus bus;
  bus.mem[0x8005] = 0x81;
  load(bus, {0xDD, 0x21, 0x00, 0x80, 0xDD, 0xCB, 0x05, 0x06});
  rainbow::Z80 cpu(bus);
  cpu.step(); // LD IX,8000h
  cpu.step(); // RLC (IX+5)
  CHECK_EQ(bus.mem[0x8005], 0x03);
  CHECK(cpu.regs().f & CF);
  CHECK_EQ(cpu.wz(), 0x8005);
}

TEST(z80_ei_delay) {
  FlatBus bus;
  load(bus, {0xFB, 0x00, 0x00}); // EI : NOP : NOP
  rainbow::Z80 cpu(bus);

  cpu.step(); // EI
  CHECK(cpu.regs().iff1);
  CHECK(!cpu.interrupt()); // the instruction after EI must still run

  cpu.step(); // NOP
  CHECK(cpu.interrupt());
}

TEST(z80_im1_interrupt) {
  FlatBus bus;
  rainbow::Z80 cpu(bus);
  cpu.regs().pc = 0x8000;
  cpu.regs().sp = 0xFF00;
  cpu.regs().iff1 = true;
  cpu.regs().iff2 = true;
  cpu.regs().im = 1;

  CHECK(cpu.interrupt());
  CHECK_EQ(cpu.regs().pc, 0x0038);
  CHECK_EQ(cpu.regs().sp, 0xFEFE);
  CHECK_EQ(bus.mem[0xFEFE], 0x00);
  CHECK_EQ(bus.mem[0xFEFF], 0x80);
  CHECK(!cpu.regs().iff1);
}

TEST(z80_im2_interrupt) {
  FlatBus bus;
  rainbow::Z80 cpu(bus);
  cpu.regs().pc = 0x8000;
  cpu.regs().sp = 0xFF00;
  cpu.regs().i = 0x40;
  cpu.regs().iff1 = true;
  cpu.regs().im = 2;

  CHECK(cpu.interrupt());
  CHECK_EQ(cpu.regs().pc, 0x40FF);
}

TEST(z80_nmi) {
  FlatBus bus;
  rainbow::Z80 cpu(bus);
  cpu.regs().pc = 0x8000;
  cpu.regs().sp = 0xFF00;
  cpu.regs().iff1 = true;

  cpu.nmi();
  CHECK_EQ(cpu.regs().pc, 0x0066);
  CHECK_EQ(cpu.regs().sp, 0xFEFE);
  CHECK_EQ(bus.mem[0xFEFE], 0x00);
  CHECK_EQ(bus.mem[0xFEFF], 0x80);
  CHECK(!cpu.regs().iff1);
}

TEST(z80_halt) {
  FlatBus bus;
  load(bus, {0x76});
  rainbow::Z80 cpu(bus);
  CHECK_EQ(cpu.step(), 4);
  CHECK(cpu.regs().halted);
  CHECK_EQ(cpu.step(), 4);
  CHECK_EQ(cpu.regs().pc, 1);
}

TEST(z80_out_and_in) {
  FlatBus bus;
  bus.io[0x1234] = 0x5A;
  load(bus, {0x01, 0x34, 0x12, 0x0E, 0x34, 0x3E, 0xAA, 0xED, 0x79, 0xED, 0x78});
  rainbow::Z80 cpu(bus); // LD BC,1234h : LD C,34h : LD A,AAh : OUT (C),A : IN A,(C)
  cpu.step();
  cpu.step();
  cpu.step();
  cpu.step();
  CHECK_EQ(bus.lastOutPort, 0x1234);
  CHECK_EQ(bus.lastOutValue, 0xAA);

  cpu.step();
  CHECK_EQ(cpu.regs().a, 0x5A);
}

TEST(z80_push_pop) {
  FlatBus bus;
  load(bus, {0x01, 0x34, 0x12, 0xC5, 0xD1}); // LD BC,1234h : PUSH BC : POP DE
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.regs().sp = 0xFF00;
  cpu.step();
  CHECK_EQ(cpu.regs().sp, 0xFEFE);
  CHECK_EQ(bus.mem[0xFEFE], 0x34);
  CHECK_EQ(bus.mem[0xFEFF], 0x12);

  cpu.step();
  CHECK_EQ(cpu.de(), 0x1234);
  CHECK_EQ(cpu.regs().sp, 0xFF00);
}

TEST(z80_conditional_jump) {
  FlatBus bus;
  load(bus, {0xAF, 0xCA, 0x00, 0x80, 0x00}); // XOR A (Z=1) : JP Z,8000h : NOP
  rainbow::Z80 cpu(bus);
  cpu.step();
  cpu.step();
  CHECK_EQ(cpu.regs().pc, 0x8000);
}

TEST(z80_jr_conditions) {
  // JR encodes only NZ/Z/NC/C, so the condition must be masked with 3 rather
  // than 7. Getting this wrong makes every JR behave like PO/PE/P/M.
  struct Case {
    rainbow::u8 op;
    rainbow::u8 flags;
    bool taken;
  };
  const Case cases[] = {
      {0x20, 0x00, true},  // JR NZ taken when Z=0
      {0x20, 0x40, false}, // JR NZ not taken when Z=1
      {0x28, 0x40, true},  // JR Z taken when Z=1
      {0x28, 0x00, false}, // JR Z not taken when Z=0
      {0x30, 0x00, true},  // JR NC taken when C=0
      {0x30, 0x01, false}, // JR NC not taken when C=1
      {0x38, 0x01, true},  // JR C taken when C=1
      {0x38, 0x00, false}, // JR C not taken when C=0
  };
  for (const Case& c : cases) {
    FlatBus bus;
    load(bus, {c.op, 0x02, 0x00}); // JR cc,+2 : NOP
    rainbow::Z80 cpu(bus);
    cpu.regs().f = c.flags;
    const rainbow::u32 t = cpu.step();
    CHECK_EQ(t, c.taken ? 12u : 7u);
    CHECK_EQ(cpu.regs().pc, c.taken ? 0x0004 : 0x0002);
  }
}

TEST(z80_jp_conditions) {
  // JP cc uses the full three-bit condition code, including PO/PE/P/M.
  struct Case {
    rainbow::u8 op;
    rainbow::u8 flags;
    bool taken;
  };
  const Case cases[] = {
      {0xC2, 0x00, true},  // JP NZ
      {0xC2, 0x40, false}, // JP NZ
      {0xCA, 0x40, true},  // JP Z
      {0xD2, 0x00, true},  // JP NC
      {0xD2, 0x01, false}, // JP NC
      {0xDA, 0x01, true},  // JP C
      {0xE2, 0x00, true},  // JP PO (P/V=0)
      {0xE2, 0x04, false}, // JP PO (P/V=1)
      {0xEA, 0x04, true},  // JP PE
      {0xF2, 0x00, true},  // JP P  (S=0)
      {0xF2, 0x80, false}, // JP P  (S=1)
      {0xFA, 0x80, true},  // JP M
  };
  for (const Case& c : cases) {
    FlatBus bus;
    load(bus, {c.op, 0x00, 0x80, 0x00}); // JP cc,8000h : NOP
    rainbow::Z80 cpu(bus);
    cpu.regs().f = c.flags;
    const rainbow::u32 t = cpu.step();
    CHECK_EQ(t, 10u);
    CHECK_EQ(cpu.regs().pc, c.taken ? 0x8000 : 0x0003);
  }
}

TEST(z80_memory_operand_hl) {
  FlatBus bus;
  bus.mem[0x8000] = 0x42;
  // LD HL,8000h : LD A,(HL) : INC (HL) : LD (HL),A : ADD A,(HL) : LD (HL),55h
  load(bus, {0x21, 0x00, 0x80, 0x7E, 0x34, 0x77, 0x86, 0x36, 0x55});
  rainbow::Z80 cpu(bus);
  cpu.step();
  CHECK_EQ(cpu.regs().pc, 0x0003);

  CHECK_EQ(cpu.step(), 7); // LD A,(HL)
  CHECK_EQ(cpu.regs().a, 0x42);
  CHECK_EQ(cpu.regs().pc, 0x0004);

  CHECK_EQ(cpu.step(), 11); // INC (HL)
  CHECK_EQ(bus.mem[0x8000], 0x43);

  CHECK_EQ(cpu.step(), 7); // LD (HL),A
  CHECK_EQ(bus.mem[0x8000], 0x42);

  CHECK_EQ(cpu.step(), 7); // ADD A,(HL)
  CHECK_EQ(cpu.regs().a, 0x84);

  CHECK_EQ(cpu.step(), 10); // LD (HL),n
  CHECK_EQ(bus.mem[0x8000], 0x55);
  CHECK_EQ(cpu.regs().pc, 0x0009);
}

TEST(z80_indexed_memory_operand) {
  FlatBus bus;
  bus.mem[0x8002] = 0x77;
  bus.mem[0x8001] = 0x10;
  // LD IX,8000h : LD A,(IX+2) : LD (IX+3),A : INC (IX+1)
  load(bus, {0xDD, 0x21, 0x00, 0x80, 0xDD, 0x7E, 0x02, 0xDD, 0x77, 0x03,
             0xDD, 0x34, 0x01});
  rainbow::Z80 cpu(bus);
  CHECK_EQ(cpu.step(), 14); // LD IX,nn
  CHECK_EQ(cpu.regs().ix, 0x8000);

  CHECK_EQ(cpu.step(), 19); // LD A,(IX+d)
  CHECK_EQ(cpu.regs().a, 0x77);

  CHECK_EQ(cpu.step(), 19); // LD (IX+d),A
  CHECK_EQ(bus.mem[0x8003], 0x77);

  CHECK_EQ(cpu.step(), 23); // INC (IX+d)
  CHECK_EQ(bus.mem[0x8001], 0x11);
  CHECK_EQ(cpu.regs().pc, 0x000D);
}
