#include "core/z80.h"

namespace mynext {

// ===========================================================================
// Construction and reset
// ===========================================================================

void Z80::reset() {
  regs_ = Registers{};
  regs_.a = 0xFF;
  regs_.f = 0xFF;
  regs_.sp = 0xFFFF;
  regs_.pc = 0x0000;
  regs_.im = 0;
  regs_.iff1 = regs_.iff2 = false;
  regs_.halted = false;
  wz_ = 0;
  cycles_ = 0;
  eiDelay_ = 0;
}

// ===========================================================================
// Bus helpers
// ===========================================================================

u8 Z80::fetch() {
  const u32 t = bus_.now();
  const u8 value = bus_.memRead(regs_.pc, t);
  const u8 wait = bus_.contention(regs_.pc, t);
  tick(4 + wait);
  ++regs_.pc;
  incR();
  return value;
}

u8 Z80::fetchImm() {
  const u8 value = read8(regs_.pc);
  ++regs_.pc;
  return value;
}

u16 Z80::fetchImm16() {
  const u8 lo = fetchImm();
  const u8 hi = fetchImm();
  return u16(hi) << 8 | lo;
}

u8 Z80::read8(u16 addr) {
  const u32 t = bus_.now();
  const u8 value = bus_.memRead(addr, t);
  const u8 wait = bus_.contention(addr, t);
  tick(3 + wait);
  return value;
}

void Z80::write8(u16 addr, u8 value) {
  const u32 t = bus_.now();
  const u8 wait = bus_.contention(addr, t);
  bus_.memWrite(addr, value, t);
  tick(3 + wait);
}

u8 Z80::in8(u16 port) {
  const u8 value = bus_.ioRead(port, bus_.now());
  tick(4);
  return value;
}

void Z80::out8(u16 port, u8 value) {
  bus_.ioWrite(port, value, bus_.now());
  tick(4);
}

// ===========================================================================
// Register helpers
// ===========================================================================

u8 Z80::getReg(int code, int idx) {
  switch (code) {
    case 0: return regs_.b;
    case 1: return regs_.c;
    case 2: return regs_.d;
    case 3: return regs_.e;
    case 4:
      if (idx == 1) return u8(regs_.ix >> 8);
      if (idx == 2) return u8(regs_.iy >> 8);
      return regs_.h;
    case 5:
      if (idx == 1) return u8(regs_.ix);
      if (idx == 2) return u8(regs_.iy);
      return regs_.l;
    case 7: return regs_.a;
    default: return 0;
  }
}

void Z80::setReg(int code, int idx, u8 value) {
  switch (code) {
    case 0: regs_.b = value; break;
    case 1: regs_.c = value; break;
    case 2: regs_.d = value; break;
    case 3: regs_.e = value; break;
    case 4:
      if (idx == 1) regs_.ix = u16(regs_.ix & 0x00FF) | u16(value) << 8;
      else if (idx == 2) regs_.iy = u16(regs_.iy & 0x00FF) | u16(value) << 8;
      else regs_.h = value;
      break;
    case 5:
      if (idx == 1) regs_.ix = u16(regs_.ix & 0xFF00) | value;
      else if (idx == 2) regs_.iy = u16(regs_.iy & 0xFF00) | value;
      else regs_.l = value;
      break;
    case 7: regs_.a = value; break;
    default: break;
  }
}

u16 Z80::hlValue(int idx) const {
  if (idx == 1) return regs_.ix;
  if (idx == 2) return regs_.iy;
  return u16(regs_.h) << 8 | regs_.l;
}

void Z80::setHlValue(int idx, u16 value) {
  if (idx == 1) regs_.ix = value;
  else if (idx == 2) regs_.iy = value;
  else { regs_.h = u8(value >> 8); regs_.l = u8(value); }
}

u16 Z80::indexAddrWithDisp(int idx, i8 d) {
  const u16 base = (idx == 1) ? regs_.ix : (idx == 2 ? regs_.iy : hl());
  wz_ = u16(base + u16(i16(d)));
  return wz_;
}

u16 Z80::indexAddr(int idx) {
  // Plain (HL) operands take no displacement and no extra T-states.
  if (idx == 0) return hl();
  const i8 d = i8(fetchImm());
  tick(5);
  return indexAddrWithDisp(idx, d);
}

// ===========================================================================
// ALU
// ===========================================================================

void Z80::add8(u8 value, u8 carry) {
  const u16 sum = u16(regs_.a) + value + carry;
  const u8 r = u8(sum);
  regs_.f = u8(szxy(r) |
               ((regs_.a ^ value ^ r) & HF) |
               (((~(regs_.a ^ value) & (regs_.a ^ r)) >> 5) & PF) |
               ((sum >> 8) & CF));
  regs_.a = r;
}

void Z80::sub8(u8 value, u8 carry) {
  const u16 diff = u16(regs_.a) - value - carry;
  const u8 r = u8(diff);
  regs_.f = u8(szxy(r) | NF |
               ((regs_.a ^ value ^ r) & HF) |
               ((((regs_.a ^ value) & (regs_.a ^ r)) >> 5) & PF) |
               ((diff >> 8) & CF));
  regs_.a = r;
}

void Z80::and8(u8 value) {
  regs_.a &= value;
  regs_.f = u8(szxyp(regs_.a) | HF);
}

void Z80::or8(u8 value) {
  regs_.a |= value;
  regs_.f = szxyp(regs_.a);
}

void Z80::xor8(u8 value) {
  regs_.a ^= value;
  regs_.f = szxyp(regs_.a);
}

void Z80::cp8(u8 value) {
  const u16 diff = u16(regs_.a) - value;
  const u8 r = u8(diff);
  regs_.f = u8((r & SF) | (r == 0 ? ZF : 0) | (value & (YF | XF)) | NF |
               ((regs_.a ^ value ^ r) & HF) |
               ((((regs_.a ^ value) & (regs_.a ^ r)) >> 5) & PF) |
               ((diff >> 8) & CF));
}

void Z80::inc8(u8& r) {
  const u8 before = r;
  r = u8(before + 1);
  regs_.f = u8((regs_.f & CF) | szxy(r) |
               ((before & 0x0F) == 0x0F ? HF : 0) |
               (before == 0x7F ? PF : 0));
}

void Z80::dec8(u8& r) {
  const u8 before = r;
  r = u8(before - 1);
  regs_.f = u8((regs_.f & CF) | szxy(r) | NF |
               ((before & 0x0F) == 0x00 ? HF : 0) |
               (before == 0x80 ? PF : 0));
}

void Z80::add16(u16& dest, u16 value) {
  const u32 sum = u32(dest) + value;
  regs_.f = u8((regs_.f & (SF | ZF | PF)) |
               (((dest ^ value ^ u16(sum)) >> 8) & HF) |
               ((sum >> 16) & CF));
  dest = u16(sum);
  wz_ = dest;
}

void Z80::adc16(u16& dest, u16 value) {
  const u32 sum = u32(dest) + value + (regs_.f & CF);
  const u16 r = u16(sum);
  regs_.f = u8(((r >> 8) & (SF | YF | XF)) | (r == 0 ? ZF : 0) |
               (((dest ^ value ^ r) >> 8) & HF) |
               (((~(dest ^ value) & (dest ^ r)) >> 13) & PF) |
               ((sum >> 16) & CF));
  dest = r;
  wz_ = dest;
}

void Z80::sbc16(u16& dest, u16 value) {
  const u32 diff = u32(dest) - value - (regs_.f & CF);
  const u16 r = u16(diff);
  regs_.f = u8(((r >> 8) & (SF | YF | XF)) | (r == 0 ? ZF : 0) | NF |
               (((dest ^ value ^ r) >> 8) & HF) |
               ((((dest ^ value) & (dest ^ r)) >> 13) & PF) |
               ((diff >> 16) & CF));
  dest = r;
  wz_ = dest;
}

void Z80::daa() {
  const u8 old = regs_.a;
  u8 a = old;
  bool carry = (regs_.f & CF) != 0;
  if (regs_.f & NF) {
    if ((regs_.f & HF) || (a & 0x0F) > 9) a = u8(a - 6);
    if (carry || a > 0x99) { a = u8(a - 0x60); carry = true; }
  } else {
    if ((regs_.f & HF) || (a & 0x0F) > 9) a = u8(a + 6);
    if (carry || a > 0x99) { a = u8(a + 0x60); carry = true; }
  }
  regs_.f = u8(szxyp(a) | (regs_.f & NF) | ((old ^ a) & HF) | (carry ? CF : 0));
  regs_.a = a;
}

void Z80::rlca() {
  const u8 c = u8(regs_.a >> 7);
  regs_.a = u8(regs_.a << 1 | c);
  regs_.f = u8((regs_.f & (SF | ZF | PF)) | (regs_.a & (YF | XF)) | c);
}

void Z80::rrca() {
  const u8 c = u8(regs_.a & 1);
  regs_.a = u8(regs_.a >> 1 | c << 7);
  regs_.f = u8((regs_.f & (SF | ZF | PF)) | (regs_.a & (YF | XF)) | c);
}

void Z80::rla() {
  const u8 c = u8(regs_.a >> 7);
  regs_.a = u8(regs_.a << 1 | (regs_.f & CF));
  regs_.f = u8((regs_.f & (SF | ZF | PF)) | (regs_.a & (YF | XF)) | c);
}

void Z80::rra() {
  const u8 c = u8(regs_.a & 1);
  regs_.a = u8(regs_.a >> 1 | ((regs_.f & CF) << 7));
  regs_.f = u8((regs_.f & (SF | ZF | PF)) | (regs_.a & (YF | XF)) | c);
}

// ===========================================================================
// CB-prefixed helpers
// ===========================================================================

void Z80::executeRot(int group, u8& r) {
  u8 c = 0;
  switch (group) {
    case 0: c = u8(r >> 7); r = u8(r << 1 | c); break;            // RLC
    case 1: c = u8(r & 1); r = u8(r >> 1 | c << 7); break;        // RRC
    case 2: c = u8(r >> 7); r = u8(r << 1 | (regs_.f & CF)); break; // RL
    case 3: c = u8(r & 1); r = u8(r >> 1 | ((regs_.f & CF) << 7)); break; // RR
    case 4: c = u8(r >> 7); r = u8(r << 1); break;                // SLA
    case 5: c = u8(r & 1); r = u8(r >> 1 | (r & 0x80)); break;    // SRA
    case 6: c = u8(r >> 7); r = u8(r << 1 | 1); break;            // SLL (undocumented)
    case 7: c = u8(r & 1); r = u8(r >> 1); break;                 // SRL
    default: break;
  }
  regs_.f = u8(szxyp(r) | c);
}

void Z80::executeBit(int bit, u8 value, bool memory) {
  const u8 mask = u8(1u << bit);
  const u8 masked = u8(value & mask);
  u8 f = u8((regs_.f & CF) | HF);
  if (masked == 0) f |= u8(ZF | PF);
  else f |= u8(masked & SF);
  if (memory) f |= u8((wz_ >> 8) & (YF | XF));
  else f |= u8(value & (YF | XF));
  regs_.f = f;
}

void Z80::executeAlu(int group, u8 value) {
  switch (group) {
    case 0: add8(value); break;
    case 1: add8(value, u8(regs_.f & CF)); break;
    case 2: sub8(value); break;
    case 3: sub8(value, u8(regs_.f & CF)); break;
    case 4: and8(value); break;
    case 5: xor8(value); break;
    case 6: or8(value); break;
    case 7: cp8(value); break;
    default: break;
  }
}

bool Z80::condition(int code) const {
  switch (code) {
    case 0: return (regs_.f & ZF) == 0;
    case 1: return (regs_.f & ZF) != 0;
    case 2: return (regs_.f & CF) == 0;
    case 3: return (regs_.f & CF) != 0;
    case 4: return (regs_.f & PF) == 0;
    case 5: return (regs_.f & PF) != 0;
    case 6: return (regs_.f & SF) == 0;
    case 7: return (regs_.f & SF) != 0;
    default: return false;
  }
}

// ===========================================================================
// CB prefix
// ===========================================================================

void Z80::executeCb() {
  const u8 op = fetch();
  const int group = op >> 6;
  const int y = (op >> 3) & 7;
  const int z = op & 7;

  if (group == 0) {
    if (z == 6) {
      const u16 addr = hlValue(0);
      u8 v = read8(addr);
      tick(1);
      executeRot(y, v);
      write8(addr, v);
    } else {
      u8 v = getReg(z, 0);
      executeRot(y, v);
      setReg(z, 0, v);
    }
  } else if (group == 1) {
    if (z == 6) {
      const u16 addr = hlValue(0);
      const u8 v = read8(addr);
      tick(1);
      executeBit(y, v, true);
    } else {
      executeBit(y, getReg(z, 0), false);
    }
  } else if (group == 2) {
    const u8 mask = u8(~(1u << y));
    if (z == 6) {
      const u16 addr = hlValue(0);
      u8 v = read8(addr);
      tick(1);
      v = u8(v & mask);
      write8(addr, v);
    } else {
      setReg(z, 0, u8(getReg(z, 0) & mask));
    }
  } else {
    const u8 mask = u8(1u << y);
    if (z == 6) {
      const u16 addr = hlValue(0);
      u8 v = read8(addr);
      tick(1);
      v = u8(v | mask);
      write8(addr, v);
    } else {
      setReg(z, 0, u8(getReg(z, 0) | mask));
    }
  }
}

// DDCB / FDCB: the displacement byte comes before the opcode and the result is
// written back to memory as well as (optionally) to a register. The register
// operand is always the unindexed one (H/L, never IXH/IXL).
void Z80::executeDdcb(int idx) {
  const i8 d = i8(fetchImm());
  const u8 op = fetch();
  const int group = op >> 6;
  const int y = (op >> 3) & 7;
  const int z = op & 7;

  const u16 base = (idx == 1) ? regs_.ix : regs_.iy;
  const u16 addr = u16(base + u16(i16(d)));
  wz_ = addr;
  tick(2);

  u8 v = read8(addr);
  switch (group) {
    case 0:
      executeRot(y, v);
      write8(addr, v);
      if (z != 6) setReg(z, 0, v);
      break;
    case 1:
      executeBit(y, v, true);
      break;
    case 2:
      v = u8(v & ~(1u << y));
      write8(addr, v);
      if (z != 6) setReg(z, 0, v);
      break;
    default:
      v = u8(v | (1u << y));
      write8(addr, v);
      if (z != 6) setReg(z, 0, v);
      break;
  }
}

// ===========================================================================
// ED prefix
// ===========================================================================

void Z80::executeEd() {
  const u8 op = fetch();

  // IN r,(C) / IN F,(C)
  if ((op & 0xC7) == 0x40) {
    const int r = (op >> 3) & 7;
    const u8 v = in8(bc());
    if (r != 6) setReg(r, 0, v);
    regs_.f = u8((regs_.f & CF) | szxyp(v));
    return;
  }

  // OUT (C),r / OUT (C),0
  if ((op & 0xC7) == 0x41) {
    const int r = (op >> 3) & 7;
    out8(bc(), r == 6 ? u8(0) : getReg(r, 0));
    return;
  }

  // SBC HL,ss
  if ((op & 0xCF) == 0x42) {
    const int ss = (op >> 4) & 3;
    const u16 v = ss == 0 ? bc() : ss == 1 ? de() : ss == 2 ? hlValue(0) : regs_.sp;
    u16 dest = hlValue(0);
    sbc16(dest, v);
    setHlValue(0, dest);
    tick(7);
    return;
  }

  // ADC HL,ss
  if ((op & 0xCF) == 0x4A) {
    const int ss = (op >> 4) & 3;
    const u16 v = ss == 0 ? bc() : ss == 1 ? de() : ss == 2 ? hlValue(0) : regs_.sp;
    u16 dest = hlValue(0);
    adc16(dest, v);
    setHlValue(0, dest);
    tick(7);
    return;
  }

  // LD (nn),ss
  if ((op & 0xCF) == 0x43) {
    const int ss = (op >> 4) & 3;
    const u16 nn = fetchImm16();
    const u16 v = ss == 0 ? bc() : ss == 1 ? de() : ss == 2 ? hlValue(0) : regs_.sp;
    write16(nn, v);
    wz_ = u16(nn + 1);
    return;
  }

  // LD ss,(nn)
  if ((op & 0xCF) == 0x4B) {
    const int ss = (op >> 4) & 3;
    const u16 nn = fetchImm16();
    const u16 v = read16(nn);
    if (ss == 0) setBc(v);
    else if (ss == 1) setDe(v);
    else if (ss == 2) setHl(v);
    else regs_.sp = v;
    wz_ = u16(nn + 1);
    return;
  }

  // Next-only opcodes. They reuse encodings that the stock Z80 treats as NOPs,
  // so the check comes first and the machine decides whether it applies.
  if (z80n_ && executeZ80n(op)) return;

  // NEG (ED 44 and its seven undocumented duplicates)
  if ((op & 0xC7) == 0x44) {
    const u8 v = regs_.a;
    regs_.a = 0;
    sub8(v);
    return;
  }

  // RETN / RETI (all eight encodings share the same low bits)
  if ((op & 0xC7) == 0x45) {
    regs_.iff1 = regs_.iff2;
    regs_.pc = pop16();
    return;
  }

  switch (op) {
    case 0x47: regs_.i = regs_.a; tick(1); return;                 // LD I,A
    case 0x4F: regs_.r = regs_.a; tick(1); return;                 // LD R,A
    case 0x57:                                                     // LD A,I
      regs_.a = regs_.i;
      tick(1);
      regs_.f = u8((regs_.f & CF) | szxy(regs_.a) | (regs_.iff2 ? PF : 0));
      return;
    case 0x5F:                                                     // LD A,R
      regs_.a = regs_.r;
      tick(1);
      regs_.f = u8((regs_.f & CF) | szxy(regs_.a) | (regs_.iff2 ? PF : 0));
      return;
    case 0x67: {                                                   // RRD
      const u16 addr = hlValue(0);
      const u8 v = read8(addr);
      tick(1);
      const u8 a = regs_.a;
      regs_.a = u8((a & 0xF0) | (v & 0x0F));
      write8(addr, u8(v >> 4 | (a << 4)));
      tick(3);
      regs_.f = u8((regs_.f & CF) | szxyp(regs_.a));
      return;
    }
    case 0x6F: {                                                   // RLD
      const u16 addr = hlValue(0);
      const u8 v = read8(addr);
      tick(1);
      const u8 a = regs_.a;
      regs_.a = u8((a & 0xF0) | (v >> 4));
      write8(addr, u8(v << 4 | (a & 0x0F)));
      tick(3);
      regs_.f = u8((regs_.f & CF) | szxyp(regs_.a));
      return;
    }
    default: break;
  }

  // IM n
  switch (op) {
    case 0x46: case 0x4E: case 0x66: case 0x6E: regs_.im = 0; return;
    case 0x56: case 0x76: regs_.im = 1; return;
    case 0x5E: case 0x7E: regs_.im = 2; return;
    default: break;
  }

  // Block transfers
  switch (op) {
    case 0xA0: case 0xA8: case 0xB0: case 0xB8: {                  // LDI/LDD/LDIR/LDDR
      const bool decrement = (op == 0xA8 || op == 0xB8);
      const bool repeat = (op == 0xB0 || op == 0xB8);
      const u8 v = read8(hlValue(0));
      write8(de(), v);
      setHlValue(0, u16(hlValue(0) + (decrement ? -1 : 1)));
      setDe(u16(de() + (decrement ? -1 : 1)));
      setBc(u16(bc() - 1));
      const u8 sum = u8(v + regs_.a);
      regs_.f = u8((regs_.f & (SF | ZF | CF)) | (bc() ? PF : 0) |
                   (sum & XF) | ((sum & 0x02) ? YF : 0));
      tick(2);
      if (repeat && bc() != 0) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return;
    }
    case 0xA1: case 0xA9: case 0xB1: case 0xB9: {                  // CPI/CPD/CPIR/CPDR
      const bool decrement = (op == 0xA9 || op == 0xB9);
      const bool repeat = (op == 0xB1 || op == 0xB9);
      const u8 v = read8(hlValue(0));
      const u8 r = u8(regs_.a - v);
      setHlValue(0, u16(hlValue(0) + (decrement ? -1 : 1)));
      setBc(u16(bc() - 1));
      const u8 hf = u8((regs_.a ^ v ^ r) & HF);
      const u8 sum = u8(r - (hf ? 1 : 0));
      regs_.f = u8((regs_.f & CF) | NF | (r & SF) | (r == 0 ? ZF : 0) | hf |
                   (bc() ? PF : 0) | (sum & XF) | ((sum & 0x02) ? YF : 0));
      tick(5);
      if (repeat && bc() != 0 && r != 0) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return;
    }
    case 0xA2: case 0xAA: case 0xB2: case 0xBA: {                  // INI/IND/INIR/INDR
      const bool decrement = (op == 0xAA || op == 0xBA);
      const bool repeat = (op == 0xB2 || op == 0xBA);
      const u8 v = in8(bc());
      write8(hlValue(0), v);
      setBc(u16(bc() - 1));
      setHlValue(0, u16(hlValue(0) + (decrement ? -1 : 1)));
      tick(2);
      if (repeat && bc() != 0) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {                  // OUTI/OUTD/OTIR/OTDR
      const bool decrement = (op == 0xAB || op == 0xBB);
      const bool repeat = (op == 0xB3 || op == 0xBB);
      const u8 v = read8(hlValue(0));
      out8(bc(), v);
      setBc(u16(bc() - 1));
      setHlValue(0, u16(hlValue(0) + (decrement ? -1 : 1)));
      tick(2);
      if (repeat && bc() != 0) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return;
    }
    default: break;
  }

  // Undocumented ED opcodes behave as NOP (8 T-states total).
  tick(0);
}

// ===========================================================================
// Z80N — the ZX Spectrum Next extension of the instruction set
// ===========================================================================

// Returns false for everything that is not a Next-only opcode, so the stock
// decoder can carry on. Timings follow the published table; the ED prefix and
// this opcode byte (8 T-states) have already been fetched by the caller.
bool Z80::executeZ80n(u8 op) {
  switch (op) {
    case 0x23: // SWAPNIB
      regs_.a = u8(regs_.a << 4 | regs_.a >> 4);
      return true;

    case 0x24: { // MIRROR A
      u8 v = regs_.a;
      v = u8((v & 0xF0) >> 4 | (v & 0x0F) << 4);
      v = u8((v & 0xCC) >> 2 | (v & 0x33) << 2);
      regs_.a = u8((v & 0xAA) >> 1 | (v & 0x55) << 1);
      return true;
    }

    case 0x27: { // TEST n — flags as AND n, A untouched
      const u8 a = regs_.a;
      and8(fetchImm());
      regs_.a = a;
      return true;
    }

    // BSLA/BSRA/BSRL/BSRF/BRLC DE,B — DE shifted by B, never more than 16 bits.
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: {
      const unsigned count = regs_.b;
      const u16 de = this->de();
      u16 result = de;
      if (count == 0) {
        result = de;
      } else if (count >= 16) {
        switch (op) {
          case 0x28: result = 0; break;                              // BSLA
          case 0x29: result = (de & 0x8000) ? u16(0xFFFF) : u16(0); break; // BSRA
          case 0x2A: result = 0; break;                              // BSRL
          case 0x2B: result = u16(0xFFFF); break;                    // BSRF
          default:                                                   // BRLC
            if ((count & 15) != 0) {
              const unsigned shift = count & 15;
              result = u16((de << shift) | (de >> (16 - shift)));
            }
            break;
        }
      } else {
        switch (op) {
          case 0x28: result = u16(de << count); break;
          case 0x29: result = u16(i16(de) >> count); break;
          case 0x2A: result = u16(de >> count); break;
          case 0x2B: result = u16((de >> count) | (0xFFFFu << (16 - count))); break;
          default: result = u16((de << count) | (de >> (16 - count))); break;
        }
      }
      setDe(result);
      return true;
    }

    case 0x30: // MUL D,E — HL = D * E, no flags
      setHl(u16(u16(regs_.d) * u16(regs_.e)));
      return true;

    case 0x31: setHl(u16(hl() + regs_.a)); return true; // ADD HL,A
    case 0x32: setDe(u16(de() + regs_.a)); return true; // ADD DE,A
    case 0x33: setBc(u16(bc() + regs_.a)); return true; // ADD BC,A

    case 0x34: case 0x35: case 0x36: { // ADD HL/DE/BC,nn
      const u16 v = fetchImm16();
      tick(2);
      if (op == 0x34) setHl(u16(hl() + v));
      else if (op == 0x35) setDe(u16(de() + v));
      else setBc(u16(bc() + v));
      return true;
    }

    case 0x8A: { // PUSH nn — the only big-endian operand in the set
      const u8 hi = fetchImm();
      const u8 lo = fetchImm();
      push16(u16(u16(hi) << 8 | lo));
      tick(3);
      return true;
    }

    case 0x90: { // OUTINB — OUTI without touching B
      out8(bc(), read8(hl()));
      setHl(u16(hl() + 1));
      tick(1);
      return true;
    }

    case 0x91: { // NEXTREG n,v
      const u8 reg = fetchImm();
      const u8 value = fetchImm();
      tick(6);
      bus_.nextRegWrite(reg, value);
      return true;
    }

    case 0x92: { // NEXTREG n,A
      const u8 reg = fetchImm();
      tick(6);
      bus_.nextRegWrite(reg, regs_.a);
      return true;
    }

    case 0x93: { // PIXELDN — HL down one pixel row inside the ULA bitmap
      u16 addr = u16(hl() + 0x0100);
      if ((addr & 0x0700) == 0) {
        const u16 l = u16((addr & 0x00FF) + 0x20);
        addr = u16((addr & 0xFF00) | (l & 0x00FF));
        if (l < 0x100) addr = u16(addr - 0x0800); // no carry: move a third
      }
      setHl(addr);
      return true;
    }

    case 0x94: { // PIXELAD — HL = ULA address of pixel (E, D)
      const u16 y = regs_.d;
      const u16 x = regs_.e;
      setHl(u16(0x4000 | ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2) |
                (x >> 3)));
      return true;
    }

    case 0x95: // SETAE — A = mask for the pixel column in E
      regs_.a = u8(0x80 >> (regs_.e & 0x07));
      return true;

    case 0x98: { // JP (C) — IN A,(C) then jump inside the current page
      const u8 v = in8(bc());
      tick(1);
      regs_.pc = u16((regs_.pc & 0xFF00) | v);
      return true;
    }

    // LDIX/LDDX/LDIRX/LDDRX: copy unless the byte equals A, no flags at all.
    case 0xA4: case 0xAC: case 0xB4: case 0xBC: {
      const bool decrement = (op == 0xAC || op == 0xBC);
      const bool repeat = (op == 0xB4 || op == 0xBC);
      const u8 v = read8(hl());
      if (v != regs_.a) write8(de(), v);
      setDe(u16(de() + 1));
      setHl(u16(hl() + (decrement ? -1 : 1)));
      setBc(u16(bc() - 1));
      tick(2);
      // A repeated block transfer also stops as soon as the byte matched A.
      if (repeat && bc() != 0 && v != regs_.a) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return true;
    }

    case 0xA5: { // LDWS — vertical byte copy for Layer 2
      const u8 v = read8(hl());
      write8(de(), v);
      regs_.l = u8(regs_.l + 1);
      const u8 d = u8(regs_.d + 1);
      regs_.d = d;
      regs_.f = u8((regs_.f & CF) | (d & (SF | YF | XF)) | (d == 0 ? ZF : 0) |
                   ((d & 0x0F) == 0 ? HF : 0) | (d == 0x80 ? PF : 0));
      return true;
    }

    case 0xB7: { // LDPIRX — pattern fill from a 8-byte aligned source
      const u16 src = u16((hl() & 0xFFF8) | (regs_.e & 0x07));
      const u8 v = read8(src);
      if (v != regs_.a) write8(de(), v);
      setDe(u16(de() + 1));
      setBc(u16(bc() - 1));
      tick(2);
      if (bc() != 0) {
        regs_.pc = u16(regs_.pc - 2);
        tick(5);
      }
      return true;
    }

    default:
      return false;
  }
}

// ===========================================================================
// Main opcode dispatch
// ===========================================================================

void Z80::execute(u8 op, int idx) {
  // LD r,r' and HALT
  if (op >= 0x40 && op < 0x80) {
    if (op == 0x76) {
      regs_.halted = true;
      return;
    }
    const int dst = (op >> 3) & 7;
    const int src = op & 7;
    if (dst == 6) {
      const u16 addr = indexAddr(idx);
      write8(addr, getReg(src, 0));
    } else if (src == 6) {
      const u16 addr = indexAddr(idx);
      setReg(dst, 0, read8(addr));
    } else {
      setReg(dst, idx, getReg(src, idx));
    }
    return;
  }

  // ALU A,r
  if (op >= 0x80 && op < 0xC0) {
    const int group = (op >> 3) & 7;
    const int src = op & 7;
    if (src == 6) {
      const u16 addr = indexAddr(idx);
      executeAlu(group, read8(addr));
    } else {
      executeAlu(group, getReg(src, idx));
    }
    return;
  }

  switch (op) {
    // --- 0x00-0x0F ------------------------------------------------------
    case 0x00: break;                                              // NOP
    case 0x01: setBc(fetchImm16()); break;                         // LD BC,nn
    case 0x02: write8(bc(), regs_.a); break;                       // LD (BC),A
    case 0x03: setBc(u16(bc() + 1)); break;                        // INC BC
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: {                  // INC r
      const int r = (op >> 3) & 7;
      if (r == 6) {
        const u16 addr = indexAddr(idx);
        u8 v = read8(addr);
        tick(1);
        inc8(v);
        write8(addr, v);
      } else {
        u8 v = getReg(r, idx);
        inc8(v);
        setReg(r, idx, v);
      }
      break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {                  // DEC r
      const int r = (op >> 3) & 7;
      if (r == 6) {
        const u16 addr = indexAddr(idx);
        u8 v = read8(addr);
        tick(1);
        dec8(v);
        write8(addr, v);
      } else {
        u8 v = getReg(r, idx);
        dec8(v);
        setReg(r, idx, v);
      }
      break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: {                  // LD r,n
      const int r = (op >> 3) & 7;
      if (r == 6) {
        if (idx != 0) {
          const i8 d = i8(fetchImm());
          const u8 n = fetchImm();
          tick(2);
          write8(indexAddrWithDisp(idx, d), n);
        } else {
          write8(hlValue(0), fetchImm());
        }
      } else {
        setReg(r, idx, fetchImm());
      }
      break;
    }
    case 0x07: rlca(); break;
    case 0x08: {
      const u8 a = regs_.a, f = regs_.f;
      regs_.a = regs_.a2; regs_.f = regs_.f2;
      regs_.a2 = a; regs_.f2 = f;
      break;
    }
    case 0x09: case 0x19: case 0x29: case 0x39: {                  // ADD HL,ss
      const int ss = (op >> 4) & 3;
      const u16 v = ss == 0 ? bc() : ss == 1 ? de() : ss == 2 ? hlValue(idx) : regs_.sp;
      u16 dest = hlValue(idx);
      add16(dest, v);
      setHlValue(idx, dest);
      tick(7);
      break;
    }
    case 0x0A: regs_.a = read8(bc()); break;                       // LD A,(BC)
    case 0x0B: setBc(u16(bc() - 1)); break;                        // DEC BC
    case 0x0F: rrca(); break;

    // --- 0x10-0x1F ------------------------------------------------------
    case 0x10: {                                                   // DJNZ d
      regs_.b = u8(regs_.b - 1);
      const i8 d = i8(fetchImm());
      tick(1);
      if (regs_.b != 0) {
        tick(5);
        regs_.pc = u16(regs_.pc + u16(i16(d)));
      }
      break;
    }
    case 0x11: setDe(fetchImm16()); break;                         // LD DE,nn
    case 0x12: write8(de(), regs_.a); break;                       // LD (DE),A
    case 0x13: setDe(u16(de() + 1)); break;                        // INC DE
    case 0x17: rla(); break;
    case 0x18: {                                                   // JR d
      const i8 d = i8(fetchImm());
      tick(5);
      regs_.pc = u16(regs_.pc + u16(i16(d)));
      break;
    }
    case 0x1A: regs_.a = read8(de()); break;                       // LD A,(DE)
    case 0x1B: setDe(u16(de() - 1)); break;                        // DEC DE
    case 0x1F: rra(); break;

    // --- 0x20-0x2F ------------------------------------------------------
    case 0x20: case 0x28: case 0x30: case 0x38: {                  // JR cc,d
      const i8 d = i8(fetchImm());
      // JR only encodes NZ/Z/NC/C, i.e. condition codes 0-3.
      if (condition((op >> 3) & 3)) {
        tick(5);
        regs_.pc = u16(regs_.pc + u16(i16(d)));
      }
      break;
    }
    case 0x21: setHlValue(idx, fetchImm16()); break;               // LD HL,nn
    case 0x22: {                                                   // LD (nn),HL
      const u16 nn = fetchImm16();
      write16(nn, hlValue(idx));
      wz_ = u16(nn + 1);
      break;
    }
    case 0x23: setHlValue(idx, u16(hlValue(idx) + 1)); break;      // INC HL
    case 0x27: daa(); break;
    case 0x2A: {                                                   // LD HL,(nn)
      const u16 nn = fetchImm16();
      setHlValue(idx, read16(nn));
      wz_ = u16(nn + 1);
      break;
    }
    case 0x2B: setHlValue(idx, u16(hlValue(idx) - 1)); break;      // DEC HL
    case 0x2F:                                                     // CPL
      regs_.a = u8(~regs_.a);
      regs_.f = u8((regs_.f & (SF | ZF | PF | CF)) | HF | NF |
                   (regs_.a & (YF | XF)));
      break;

    // --- 0x30-0x3F ------------------------------------------------------
    case 0x31: regs_.sp = fetchImm16(); break;                     // LD SP,nn
    case 0x32: {                                                   // LD (nn),A
      const u16 nn = fetchImm16();
      write8(nn, regs_.a);
      wz_ = u16(u16(regs_.a) << 8 | u8(nn + 1));
      break;
    }
    case 0x33: ++regs_.sp; break;                                  // INC SP
    case 0x37:                                                     // SCF
      regs_.f = u8((regs_.f & (SF | ZF | PF)) | CF | (regs_.a & (YF | XF)));
      break;
    case 0x3A: {                                                   // LD A,(nn)
      const u16 nn = fetchImm16();
      regs_.a = read8(nn);
      wz_ = u16(nn + 1);
      break;
    }
    case 0x3B: --regs_.sp; break;                                  // DEC SP
    case 0x3F: {                                                   // CCF
      const u8 c = u8(regs_.f & CF);
      regs_.f = u8((regs_.f & (SF | ZF | PF)) | (c ? HF : CF) |
                   (regs_.a & (YF | XF)));
      break;
    }

    // --- 0xC0-0xCF ------------------------------------------------------
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
    case 0xE0: case 0xE8: case 0xF0: case 0xF8: {                  // RET cc
      tick(1);
      if (condition((op >> 3) & 7)) regs_.pc = pop16();
      break;
    }
    case 0xC1: setBc(pop16()); break;                              // POP BC
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA: {                  // JP cc,nn
      const u16 nn = fetchImm16();
      if (condition((op >> 3) & 7)) {
        regs_.pc = nn;
        wz_ = nn;
      }
      break;
    }
    case 0xC3: {                                                   // JP nn
      const u16 nn = fetchImm16();
      regs_.pc = nn;
      wz_ = nn;
      break;
    }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC: {                  // CALL cc,nn
      const u16 nn = fetchImm16();
      if (condition((op >> 3) & 7)) {
        tick(1);
        push16(regs_.pc);
        regs_.pc = nn;
        wz_ = nn;
      }
      break;
    }
    case 0xC5: tick(1); push16(bc()); break;                       // PUSH BC
    case 0xC6: executeAlu(0, fetchImm()); break;                   // ADD A,n
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF: {                  // RST n
      tick(1);
      push16(regs_.pc);
      regs_.pc = u16(op & 0x38);
      wz_ = regs_.pc;
      break;
    }
    case 0xC9: regs_.pc = pop16(); break;                          // RET
    case 0xCB: executeDdcb(idx); break;                            // DD CB d op / FD CB d op
    case 0xCD: {                                                   // CALL nn
      const u16 nn = fetchImm16();
      tick(1);
      push16(regs_.pc);
      regs_.pc = nn;
      wz_ = nn;
      break;
    }
    case 0xCE: executeAlu(1, fetchImm()); break;                   // ADC A,n

    // --- 0xD0-0xDF ------------------------------------------------------
    case 0xD1: setDe(pop16()); break;                              // POP DE
    case 0xD3: {                                                   // OUT (n),A
      const u8 n = fetchImm();
      out8(u16(u16(regs_.a) << 8 | n), regs_.a);
      wz_ = u16(u16(regs_.a) << 8 | u8(n + 1));
      break;
    }
    case 0xD5: tick(1); push16(de()); break;                       // PUSH DE
    case 0xD6: executeAlu(2, fetchImm()); break;                   // SUB n
    case 0xD9: {                                                   // EXX
      const u16 bcv = bc(), dev = de(), hlv = hlValue(0);
      setBc(u16(regs_.b2) << 8 | regs_.c2);
      setDe(u16(regs_.d2) << 8 | regs_.e2);
      setHl(u16(regs_.h2) << 8 | regs_.l2);
      regs_.b2 = u8(bcv >> 8); regs_.c2 = u8(bcv);
      regs_.d2 = u8(dev >> 8); regs_.e2 = u8(dev);
      regs_.h2 = u8(hlv >> 8); regs_.l2 = u8(hlv);
      break;
    }
    case 0xDB: {                                                   // IN A,(n)
      const u8 n = fetchImm();
      regs_.a = in8(u16(u16(regs_.a) << 8 | n));
      wz_ = u16(u16(regs_.a) << 8 | u8(n + 1));
      break;
    }
    case 0xDE: executeAlu(3, fetchImm()); break;                   // SBC A,n
    case 0xED: executeEd(); break;                                 // DD ED ... -> ED ignored prefix

    // --- 0xE0-0xEF ------------------------------------------------------
    case 0xE1: setHlValue(idx, pop16()); break;                    // POP HL
    case 0xE3: {                                                   // EX (SP),HL
      const u16 tmp = read16(regs_.sp);
      write16(regs_.sp, hlValue(idx));
      setHlValue(idx, tmp);
      tick(3);
      wz_ = tmp;
      break;
    }
    case 0xE5: tick(1); push16(hlValue(idx)); break;               // PUSH HL
    case 0xE6: executeAlu(4, fetchImm()); break;                   // AND n
    case 0xE9: regs_.pc = hlValue(idx); break;                     // JP (HL)
    case 0xEB: {                                                   // EX DE,HL
      const u16 d = de(), h = hlValue(0);
      setDe(h);
      setHl(d);
      break;
    }
    case 0xEE: executeAlu(5, fetchImm()); break;                   // XOR n

    // --- 0xF0-0xFF ------------------------------------------------------
    case 0xF1: {                                                   // POP AF
      const u16 v = pop16();
      setAf(v);
      break;
    }
    case 0xF3: regs_.iff1 = regs_.iff2 = false; break;             // DI
    case 0xF5: tick(1); push16(af()); break;                       // PUSH AF
    case 0xF6: executeAlu(6, fetchImm()); break;                   // OR n
    case 0xF9: regs_.sp = hlValue(idx); break;                     // LD SP,HL
    case 0xFB:                                                     // EI
      regs_.iff1 = regs_.iff2 = true;
      eiDelay_ = 1;
      break;
    case 0xFE: executeAlu(7, fetchImm()); break;                   // CP n

    default:
      // Unreachable: every documented opcode is handled above. The DD/FD/ED/CB
      // prefixes are dispatched from step().
      break;
  }
}

// ===========================================================================
// Instruction step and interrupts
// ===========================================================================

u32 Z80::step() {
  const u64 start = cycles_;

  // EI takes effect only after the instruction that follows it.
  if (eiDelay_ > 0) --eiDelay_;

  if (regs_.halted) {
    tick(4);
    return u32(cycles_ - start);
  }

  const u8 op = fetch();
  if (op == 0xDD || op == 0xFD) {
    int idx = (op == 0xDD) ? 1 : 2;
    u8 op2 = fetch();
    while (op2 == 0xDD || op2 == 0xFD) {
      idx = (op2 == 0xDD) ? 1 : 2;
      op2 = fetch();
    }
    execute(op2, idx);
  } else if (op == 0xED) {
    executeEd();
  } else if (op == 0xCB) {
    executeCb();
  } else {
    execute(op, 0);
  }

  return u32(cycles_ - start);
}

bool Z80::interrupt() {
  if (!regs_.iff1 || eiDelay_ > 0) return false;

  regs_.iff1 = regs_.iff2 = false;
  regs_.halted = false;

  if (regs_.im == 2) {
    const u8 vector = bus_.interruptVector();
    tick(7);
    push16(regs_.pc);
    regs_.pc = u16(u16(regs_.i) << 8 | vector);
    tick(6);
  } else {
    tick(7);
    push16(regs_.pc);
    regs_.pc = 0x0038;
  }
  return true;
}

void Z80::nmi() {
  regs_.iff1 = false;
  regs_.halted = false;
  tick(5);
  push16(regs_.pc);
  regs_.pc = 0x0066;
  tick(6);
}

} // namespace mynext
