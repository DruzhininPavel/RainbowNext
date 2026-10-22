#pragma once

#include "core/bus.h"
#include "core/types.h"

namespace rainbow {

// A complete Z80 interpreter: documented and undocumented instructions,
// IX/IY prefix quirks, MEMPTR (WZ), interrupt modes 0/1/2, NMI and HALT.
// The core is deliberately free of any Spectrum-specific knowledge: all
// memory and I/O traffic goes through Bus, and all timing is expressed in
// T-states so that contended machines can insert wait states.
class Z80 {
public:
  struct Registers {
    u8 a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    u8 a2 = 0, f2 = 0, b2 = 0, c2 = 0, d2 = 0, e2 = 0, h2 = 0, l2 = 0;
    u16 ix = 0, iy = 0, sp = 0, pc = 0;
    u8 i = 0, r = 0;
    bool iff1 = false, iff2 = false;
    u8 im = 1;
    bool halted = false;

    template <class Ar> void serialize(Ar& ar) {
      ar(a); ar(f); ar(b); ar(c); ar(d); ar(e); ar(h); ar(l);
      ar(a2); ar(f2); ar(b2); ar(c2); ar(d2); ar(e2); ar(h2); ar(l2);
      ar(ix); ar(iy); ar(sp); ar(pc);
      ar(i); ar(r); ar(iff1); ar(iff2); ar(im); ar(halted);
    }
  };

  explicit Z80(Bus& bus) : bus_(bus) { reset(); }

  void reset();

  // Execute exactly one instruction (or one iteration of a block instruction).
  // Returns the number of T-states consumed, including contention waits.
  u32 step();

  // Maskable interrupt acknowledge. Returns true if the interrupt was taken.
  bool interrupt();

  // Non-maskable interrupt.
  void nmi();

  // Direct register access is used by snapshots and the debugger.
  Registers& regs() { return regs_; }
  const Registers& regs() const { return regs_; }

  u16 af() const { return u16(regs_.a) << 8 | regs_.f; }
  u16 bc() const { return u16(regs_.b) << 8 | regs_.c; }
  u16 de() const { return u16(regs_.d) << 8 | regs_.e; }
  u16 hl() const { return u16(regs_.h) << 8 | regs_.l; }
  void setAf(u16 v) { regs_.a = u8(v >> 8); regs_.f = u8(v); }
  void setBc(u16 v) { regs_.b = u8(v >> 8); regs_.c = u8(v); }
  void setDe(u16 v) { regs_.d = u8(v >> 8); regs_.e = u8(v); }
  void setHl(u16 v) { regs_.h = u8(v >> 8); regs_.l = u8(v); }

  u64 cycles() const { return cycles_; }
  u16 wz() const { return wz_; }

  // Z80N is the Next's extension of the instruction set. On the classic
  // machines the same opcodes decode as NOPs, so the core enables them only
  // when the machine says so.
  void setExtendedInstructions(bool on) { z80n_ = on; }
  bool extendedInstructions() const { return z80n_; }

  // Save states carry the registers, MEMPTR, the EI delay and the cycle
  // counter; the bus belongs to the machine and is not part of them.
  template <class Ar> void serialize(Ar& ar) {
    regs_.serialize(ar);
    ar(wz_);
    ar(cycles_);
    ar(eiDelay_);
    ar(z80n_);
  }

private:
  // --- clock and memory ---------------------------------------------------
  void tick(u32 n) { bus_.tick(n); cycles_ += n; }
  u8 fetch();          // M1 opcode fetch (4 T + contention, increments R)
  u8 fetchImm();       // immediate byte (3 T + contention)
  u16 fetchImm16();    // two immediate bytes (6 T + contention)
  u8 read8(u16 addr);
  void write8(u16 addr, u8 value);
  u16 read16(u16 addr) { u8 lo = read8(addr); u8 hi = read8(addr + 1); return u16(hi) << 8 | lo; }
  void write16(u16 addr, u16 value) { write8(addr, u8(value)); write8(addr + 1, u8(value >> 8)); }
  u8 in8(u16 port);
  void out8(u16 port, u8 value);
  void push16(u16 value) { regs_.sp -= 2; write16(regs_.sp, value); }
  u16 pop16() { u16 v = read16(regs_.sp); regs_.sp += 2; return v; }
  void incR() { regs_.r = u8((regs_.r & 0x80) | ((regs_.r + 1) & 0x7F)); }

  // --- registers ----------------------------------------------------------
  u8 getReg(int code, int idx);
  void setReg(int code, int idx, u8 value);
  u16 hlValue(int idx) const;
  void setHlValue(int idx, u16 value);
  u16 indexAddr(int idx);          // (IX+d) / (IY+d), fetches d and ticks
  u16 indexAddrWithDisp(int idx, i8 d);

  // --- flags and ALU ------------------------------------------------------
  static constexpr u8 SF = 0x80, ZF = 0x40, YF = 0x20, HF = 0x10;
  static constexpr u8 XF = 0x08, PF = 0x04, NF = 0x02, CF = 0x01;

  static u8 parity(u8 v) {
    v ^= u8(v >> 4);
    v ^= u8(v >> 2);
    v ^= u8(v >> 1);
    return u8(~v & 1);
  }
  static u8 szxy(u8 v) { return u8((v & (SF | YF | XF)) | (v == 0 ? ZF : 0)); }
  static u8 szxyp(u8 v) { return u8(szxy(v) | (parity(v) ? PF : 0)); }

  void add8(u8 value, u8 carry = 0);
  void sub8(u8 value, u8 carry = 0);
  void and8(u8 value);
  void or8(u8 value);
  void xor8(u8 value);
  void cp8(u8 value);
  void inc8(u8& r);
  void dec8(u8& r);
  void add16(u16& dest, u16 value);
  void adc16(u16& dest, u16 value);
  void sbc16(u16& dest, u16 value);
  void daa();
  void rlca();
  void rrca();
  void rla();
  void rra();

  // --- instruction groups -------------------------------------------------
  void execute(u8 op, int idx);
  void executeCb();
  void executeEd();
  bool executeZ80n(u8 op);
  void executeDdcb(int idx);
  void executeAlu(int group, u8 value);
  void executeRot(int group, u8& r);      // CB rotates/shifts
  void executeBit(int bit, u8 value, bool memory);
  bool condition(int code) const;

  Bus& bus_;
  Registers regs_;
  u16 wz_ = 0;
  u64 cycles_ = 0;
  int eiDelay_ = 0;
  bool z80n_ = false;
};

} // namespace rainbow
