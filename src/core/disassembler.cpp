#include "core/disassembler.h"

#include <cstdio>
#include <string>
#include <utility>

namespace rainbow {
namespace {

// The decoder pulls bytes in the order the CPU fetches them, so an immediate
// or a displacement appears in the listing where the opcode expects it and the
// length falls out of how many bytes were consumed.
class Decoder {
public:
  Decoder(u16 address, const ByteReader& read) : address_(address), read_(read) {}

  u8 next() { return read_(u16(address_ + length_++)); }
  u16 next16() {
    const u8 lo = next();
    const u8 hi = next();
    return u16(u16(hi) << 8 | lo);
  }
  i8 nextDisp() { return i8(next()); }
  // Where the instruction ends, which is also the base for a relative jump.
  u16 after() const { return u16(address_ + length_); }
  int length() const { return length_; }

private:
  u16 address_;
  const ByteReader& read_;
  int length_ = 0;
};

const char* const kCond[8] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
// The forms that name A already carry the separator, so the operand is just
// appended: "ADD A,B" but "SUB B".
const char* const kAlu[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,",
                             "AND ",   "XOR ",   "OR ",  "CP "};
const char* const kRot[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
const char* const kReg8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
const char* const kReg16[4] = {"BC", "DE", "HL", "SP"};
const char* const kStackReg[4] = {"BC", "DE", "HL", "AF"};

std::string hex16(u16 value) {
  char text[8];
  std::snprintf(text, sizeof text, "0x%04X", value);
  return text;
}

std::string hex8(u8 value) {
  char text[6];
  std::snprintf(text, sizeof text, "0x%02X", value);
  return text;
}

std::string signedText(int value) {
  char text[8];
  std::snprintf(text, sizeof text, "%+d", value);
  return text;
}

// The prefix replaces HL with IX or IY; index 0 means no prefix at all.
const char* indexName(int idx) { return idx == 1 ? "IX" : "IY"; }
const char* hlName(int idx) { return idx == 0 ? "HL" : indexName(idx); }

std::string reg8Name(int code, int idx) {
  if (idx != 0) {
    if (code == 4) return std::string(indexName(idx)) + "H";
    if (code == 5) return std::string(indexName(idx)) + "L";
  }
  return kReg8[code];
}

// (HL), or (IX+d)/(IY+d) with the displacement byte consumed.
std::string memOperand(Decoder& d, int idx) {
  if (idx == 0) return "(HL)";
  return std::string("(") + indexName(idx) + signedText(d.nextDisp()) + ")";
}

std::string reg16Name(int code, int idx) {
  if (code == 2) return hlName(idx);
  return kReg16[code];
}

// PUSH and POP put AF where LD rr,nn puts SP, and HL follows the prefix.
std::string stackRegName(int code, int idx) {
  if (code == 2) return hlName(idx);
  return kStackReg[code];
}

// The prefix only reaches the register that is not already indirect: on real
// hardware `DD 74 d` is LD (IX+d),H rather than LD (IX+d),IXH, and the CPU core
// decodes it the same way.
std::string ldRegisters(Decoder& d, int dst, int src, int idx) {
  if (dst == 6) return "LD " + memOperand(d, idx) + "," + reg8Name(src, 0);
  if (src == 6) return "LD " + reg8Name(dst, 0) + "," + memOperand(d, idx);
  return "LD " + reg8Name(dst, idx) + "," + reg8Name(src, idx);
}

std::string decodeCb(Decoder& d) {
  const u8 sub = d.next();
  const int group = sub >> 6;
  const int bit = (sub >> 3) & 7;
  const int reg = sub & 7;
  const std::string operand = reg8Name(reg, 0);

  switch (group) {
    case 0: return std::string(kRot[bit]) + " " + operand;
    case 1: return "BIT " + std::to_string(bit) + "," + operand;
    case 2: return "RES " + std::to_string(bit) + "," + operand;
    default: return "SET " + std::to_string(bit) + "," + operand;
  }
}

// DD CB d op: the displacement comes before the operation, and the operation
// also names the register it copies the result into.
std::string decodeIndexedCb(Decoder& d, int idx) {
  const i8 disp = d.nextDisp();
  const u8 sub = d.next();
  const int group = sub >> 6;
  const int bit = (sub >> 3) & 7;
  const int reg = sub & 7;
  const std::string operand =
      std::string("(") + indexName(idx) + signedText(disp) + ")";

  if (group == 1) return "BIT " + std::to_string(bit) + "," + operand;

  const char* const mnemonic = group == 0 ? kRot[bit] : (group == 2 ? "RES" : "SET");
  std::string text = std::string(mnemonic) + " ";
  if (group != 0) text += std::to_string(bit) + ",";
  text += operand;
  if (reg != 6) text += "," + reg8Name(reg, 0);
  return text;
}

std::string decodeEd(Decoder& d, bool z80n, Flow& flow) {
  const u8 op = d.next();

  if (z80n) {
    switch (op) {
      case 0x23: return "SWAPNIB";
      case 0x24: return "MIRROR A";
      case 0x27: return "TEST " + hex8(d.next());
      case 0x28: return "BSLA DE,B";
      case 0x29: return "BSRA DE,B";
      case 0x2A: return "BSRL DE,B";
      case 0x2B: return "BSRF DE,B";
      case 0x2C: return "BRLC DE,B";
      case 0x30: return "MUL D,E";
      case 0x31: return "ADD HL,A";
      case 0x32: return "ADD DE,A";
      case 0x33: return "ADD BC,A";
      case 0x34: return "ADD HL," + hex16(d.next16());
      case 0x35: return "ADD DE," + hex16(d.next16());
      case 0x36: return "ADD BC," + hex16(d.next16());
      // The only big-endian immediate in the instruction set: high byte first.
      case 0x8A: {
        const u8 hi = d.next();
        const u8 lo = d.next();
        return "PUSH " + hex16(u16(u16(hi) << 8 | lo));
      }
      case 0x90: return "OUTINB";
      case 0x91: {
        const u8 reg = d.next();
        const u8 value = d.next();
        return "NEXTREG " + hex8(reg) + "," + hex8(value);
      }
      case 0x92: return "NEXTREG " + hex8(d.next()) + ",A";
      case 0x93: return "PIXELDN";
      case 0x94: return "PIXELAD";
      case 0x95: return "SETAE";
      case 0x98:
        flow = Flow::Jump;
        return "JP (C)";
      case 0xA4: return "LDIX";
      case 0xA5: return "LDWS";
      case 0xAC: return "LDDX";
      case 0xB4: return "LDIRX";
      case 0xB7: return "LDPIRX";
      case 0xBC: return "LDDRX";
      default: break;
    }
  }

  // IN r,(C) / OUT (C),r
  if ((op & 0xC7) == 0x40) {
    const int reg = (op >> 3) & 7;
    if (reg == 6) return "IN (C)";       // the byte is read and dropped
    return "IN " + reg8Name(reg, 0) + ",(C)";
  }
  if ((op & 0xC7) == 0x41) {
    const int reg = (op >> 3) & 7;
    if (reg == 6) return "OUT (C),0";
    return "OUT (C)," + reg8Name(reg, 0);
  }
  // SBC HL,rr / ADC HL,rr
  if ((op & 0xCF) == 0x42) {
    return "SBC HL," + std::string(kReg16[(op >> 4) & 3]);
  }
  if ((op & 0xCF) == 0x4A) {
    return "ADC HL," + std::string(kReg16[(op >> 4) & 3]);
  }
  // LD (nn),rr / LD rr,(nn)
  if ((op & 0xCF) == 0x43) {
    return "LD (" + hex16(d.next16()) + ")," + kReg16[(op >> 4) & 3];
  }
  if ((op & 0xCF) == 0x4B) {
    return "LD " + std::string(kReg16[(op >> 4) & 3]) + ",(" + hex16(d.next16()) + ")";
  }

  switch (op) {
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C:
      return "NEG";
    case 0x45: case 0x55: case 0x65: case 0x75: case 0x7D:
      flow = Flow::Return;
      return "RETN";
    case 0x4D:
      flow = Flow::Return;
      return "RETI";
    case 0x46: case 0x4E: case 0x66: case 0x6E: return "IM 0";
    case 0x56: case 0x76: return "IM 1";
    case 0x5E: case 0x7E: return "IM 2";
    case 0x47: return "LD I,A";
    case 0x4F: return "LD R,A";
    case 0x57: return "LD A,I";
    case 0x5F: return "LD A,R";
    case 0x67: return "RRD";
    case 0x6F: return "RLD";
    case 0xA0: return "LDI";
    case 0xA1: return "CPI";
    case 0xA2: return "INI";
    case 0xA3: return "OUTI";
    case 0xA8: return "LDD";
    case 0xA9: return "CPD";
    case 0xAA: return "IND";
    case 0xAB: return "OUTD";
    case 0xB0: return "LDIR";
    case 0xB1: return "CPIR";
    case 0xB2: return "INIR";
    case 0xB3: return "OTIR";
    case 0xB8: return "LDDR";
    case 0xB9: return "CPDR";
    case 0xBA: return "INDR";
    case 0xBB: return "OTDR";
    // Everything else in the ED space is two NOPs on a real Z80.
    default: return "NOP";
  }
}

std::string decodeBase(Decoder& d, u8 op, int idx, Flow& flow) {
  if (op >= 0x40 && op <= 0x7F) {
    if (op == 0x76) return "HALT";
    return ldRegisters(d, (op >> 3) & 7, op & 7, idx);
  }

  if (op >= 0x80 && op <= 0xBF) {
    const int src = op & 7;
    const std::string operand = src == 6 ? memOperand(d, idx) : reg8Name(src, idx);
    return std::string(kAlu[(op >> 3) & 7]) + operand;
  }

  if ((op & 0xC7) == 0x04) {
    const int reg = (op >> 3) & 7;
    return "INC " + (reg == 6 ? memOperand(d, idx) : reg8Name(reg, idx));
  }
  if ((op & 0xC7) == 0x05) {
    const int reg = (op >> 3) & 7;
    return "DEC " + (reg == 6 ? memOperand(d, idx) : reg8Name(reg, idx));
  }
  if ((op & 0xC7) == 0x06) {
    const int reg = (op >> 3) & 7;
    if (reg == 6) return "LD " + memOperand(d, idx) + "," + hex8(d.next());
    return "LD " + reg8Name(reg, idx) + "," + hex8(d.next());
  }

  if ((op & 0xCF) == 0x01) {
    return "LD " + reg16Name((op >> 4) & 3, idx) + "," + hex16(d.next16());
  }
  if ((op & 0xCF) == 0x03) return "INC " + std::string(reg16Name((op >> 4) & 3, idx));
  if ((op & 0xCF) == 0x0B) return "DEC " + std::string(reg16Name((op >> 4) & 3, idx));
  if ((op & 0xCF) == 0x09) {
    return "ADD " + std::string(hlName(idx)) + "," + reg16Name((op >> 4) & 3, idx);
  }

  if ((op & 0xC7) == 0xC0) {
    flow = Flow::Return;
    return "RET " + std::string(kCond[(op >> 3) & 7]);
  }
  if ((op & 0xC7) == 0xC2) {
    flow = Flow::Jump;
    return "JP " + std::string(kCond[(op >> 3) & 7]) + "," + hex16(d.next16());
  }
  if ((op & 0xC7) == 0xC4) {
    flow = Flow::Call;
    return "CALL " + std::string(kCond[(op >> 3) & 7]) + "," + hex16(d.next16());
  }
  if ((op & 0xC7) == 0xC6) {
    return std::string(kAlu[(op >> 3) & 7]) + hex8(d.next());
  }
  if ((op & 0xC7) == 0xC7) {
    flow = Flow::Call;
    return "RST " + hex8(u8(op & 0x38));
  }
  if ((op & 0xCF) == 0xC1) return "POP " + stackRegName((op >> 4) & 3, idx);
  if ((op & 0xCF) == 0xC5) return "PUSH " + stackRegName((op >> 4) & 3, idx);

  switch (op) {
    case 0x00: return "NOP";
    case 0x02: return "LD (BC),A";
    case 0x07: return "RLCA";
    case 0x08: return "EX AF,AF'";
    case 0x0A: return "LD A,(BC)";
    case 0x0F: return "RRCA";
    case 0x10: {
      const i8 e = d.nextDisp();
      flow = Flow::Jump;
      return "DJNZ " + hex16(u16(d.after() + e));
    }
    case 0x12: return "LD (DE),A";
    case 0x17: return "RLA";
    case 0x18: {
      const i8 e = d.nextDisp();
      flow = Flow::Jump;
      return "JR " + hex16(u16(d.after() + e));
    }
    case 0x1A: return "LD A,(DE)";
    case 0x1F: return "RRA";
    case 0x20: case 0x28: case 0x30: case 0x38: {
      const i8 e = d.nextDisp();
      flow = Flow::Jump;
      // JR only has the first four conditions, so it uses a two-bit field.
      return "JR " + std::string(kCond[(op >> 3) & 3]) + "," + hex16(u16(d.after() + e));
    }
    case 0x22: return "LD (" + hex16(d.next16()) + ")," + hlName(idx);
    case 0x27: return "DAA";
    case 0x2A: return "LD " + std::string(hlName(idx)) + ",(" + hex16(d.next16()) + ")";
    case 0x2F: return "CPL";
    case 0x32: return "LD (" + hex16(d.next16()) + "),A";
    case 0x37: return "SCF";
    case 0x3A: return "LD A,(" + hex16(d.next16()) + ")";
    case 0x3F: return "CCF";
    case 0xC3: {
      flow = Flow::Jump;
      return "JP " + hex16(d.next16());
    }
    case 0xC9:
      flow = Flow::Return;
      return "RET";
    case 0xCD: {
      flow = Flow::Call;
      return "CALL " + hex16(d.next16());
    }
    case 0xD3: return "OUT (" + hex8(d.next()) + "),A";
    case 0xD9: return "EXX";
    case 0xDB: return "IN A,(" + hex8(d.next()) + ")";
    case 0xE3: return "EX (SP)," + std::string(hlName(idx));
    case 0xE9:
      flow = Flow::Jump;
      return "JP (" + std::string(hlName(idx)) + ")";
    case 0xEB: return "EX DE,HL";
    case 0xF3: return "DI";
    case 0xF9: return "LD SP," + std::string(hlName(idx));
    case 0xFB: return "EI";
    default: return "NOP";
  }
}

} // namespace

Disassembled disassemble(u16 address, const ByteReader& read, bool z80n) {
  Decoder d(address, read);
  Flow flow = Flow::Next;

  u8 op = d.next();
  int idx = 0;
  // Repeated prefixes are allowed; the last one wins, as on hardware.
  while (op == 0xDD || op == 0xFD) {
    idx = op == 0xDD ? 1 : 2;
    op = d.next();
  }

  std::string text;
  if (op == 0xCB) {
    text = idx == 0 ? decodeCb(d) : decodeIndexedCb(d, idx);
  } else if (op == 0xED) {
    text = decodeEd(d, z80n, flow);
  } else {
    text = decodeBase(d, op, idx, flow);
  }

  return Disassembled{d.length(), std::move(text), flow};
}

} // namespace rainbow
