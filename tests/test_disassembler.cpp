#include "test_framework.h"

#include <array>
#include <initializer_list>
#include <string>

#include "core/disassembler.h"

namespace {

using rainbow::ByteReader;
using rainbow::Disassembled;
using rainbow::Flow;
using rainbow::u16;
using rainbow::u8;

// A flat 64K view, which is what the CPU sees once paging is resolved. The
// disassembler only ever asks for bytes through the reader, so this is enough
// to check the decoder itself.
struct Memory {
  std::array<u8, 65536> bytes{};

  ByteReader reader() const {
    return [this](u16 address) { return bytes[address]; };
  }
};

constexpr u16 kBase = 0x8000;

// Loads `code` at `address` and decodes the instruction that starts there.
Disassembled decodeAt(Memory& memory, u16 address, std::initializer_list<u8> code,
                      bool z80n = false) {
  u16 at = address;
  for (u8 byte : code) memory.bytes[at++] = byte;
  return rainbow::disassemble(address, memory.reader(), z80n);
}

Disassembled decode(Memory& memory, std::initializer_list<u8> code, bool z80n = false) {
  return decodeAt(memory, kBase, code, z80n);
}

// The four fields a listing has to get right: mnemonic, operand, length, flow.
void expect(Memory& memory, std::initializer_list<u8> code, const std::string& text,
            int length, Flow flow = Flow::Next, bool z80n = false) {
  const Disassembled result = decode(memory, code, z80n);
  CHECK_EQ(result.length, length);
  if (result.text != text) {
    ::testing::fail(__FILE__, __LINE__, "expected \"" + text + "\", got \"" + result.text + "\"");
  }
  CHECK(result.flow == flow);
}

} // namespace

TEST(disassembler_loads_eight_bit_registers) {
  Memory m;
  expect(m, {0x78}, "LD A,B", 1);
  expect(m, {0x41}, "LD B,C", 1);
  expect(m, {0x7F}, "LD A,A", 1);
  expect(m, {0x77}, "LD (HL),A", 1);
  expect(m, {0x46}, "LD B,(HL)", 1);
  expect(m, {0x76}, "HALT", 1);
}

TEST(disassembler_shows_immediates_in_fetch_order) {
  Memory m;
  expect(m, {0x3E, 0x42}, "LD A,0x42", 2);
  expect(m, {0x06, 0x00}, "LD B,0x00", 2);
  expect(m, {0x36, 0xFF}, "LD (HL),0xFF", 2);
  // The low byte is fetched first, so 0x01 0x34 0x12 is 0x1234.
  expect(m, {0x01, 0x34, 0x12}, "LD BC,0x1234", 3);
  expect(m, {0x11, 0xCD, 0xAB}, "LD DE,0xABCD", 3);
  expect(m, {0x21, 0x00, 0x40}, "LD HL,0x4000", 3);
  expect(m, {0x31, 0xFF, 0xFF}, "LD SP,0xFFFF", 3);
}

TEST(disassembler_handles_the_alu_group) {
  Memory m;
  expect(m, {0x80}, "ADD A,B", 1);
  expect(m, {0x8E}, "ADC A,(HL)", 1);
  expect(m, {0x97}, "SUB A", 1);
  expect(m, {0x9A}, "SBC A,D", 1);
  expect(m, {0xA0}, "AND B", 1);
  expect(m, {0xB1}, "OR C", 1);
  expect(m, {0xAA}, "XOR D", 1);
  expect(m, {0xBE}, "CP (HL)", 1);
  expect(m, {0xC6, 0x10}, "ADD A,0x10", 2);
  expect(m, {0xFE, 0x20}, "CP 0x20", 2);
}

TEST(disassembler_handles_the_cb_prefix) {
  Memory m;
  expect(m, {0xCB, 0x00}, "RLC B", 2);
  expect(m, {0xCB, 0x3F}, "SRL A", 2);
  expect(m, {0xCB, 0x06}, "RLC (HL)", 2);
  expect(m, {0xCB, 0x7F}, "BIT 7,A", 2);
  expect(m, {0xCB, 0x46}, "BIT 0,(HL)", 2);
  expect(m, {0xCB, 0x80}, "RES 0,B", 2);
  expect(m, {0xCB, 0xDE}, "SET 3,(HL)", 2);
}

TEST(disassembler_replaces_hl_with_ix_and_iy) {
  Memory m;
  expect(m, {0xDD, 0x21, 0x34, 0x12}, "LD IX,0x1234", 4);
  expect(m, {0xFD, 0x21, 0x34, 0x12}, "LD IY,0x1234", 4);
  expect(m, {0xDD, 0x7E, 0x05}, "LD A,(IX+5)", 3);
  expect(m, {0xFD, 0x77, 0xFF}, "LD (IY-1),A", 3);
  // The prefix never reaches the indirect side of a load.
  expect(m, {0xDD, 0x74, 0x02}, "LD (IX+2),H", 3);
  expect(m, {0xDD, 0x60}, "LD IXH,B", 2);
  expect(m, {0xFD, 0x6D}, "LD IYL,IYL", 2);
  expect(m, {0xDD, 0x86, 0x02}, "ADD A,(IX+2)", 3);
  expect(m, {0xDD, 0x34, 0x00}, "INC (IX+0)", 3);
  expect(m, {0xDD, 0xE1}, "POP IX", 2);
  expect(m, {0xDD, 0xE5}, "PUSH IX", 2);
  expect(m, {0xFD, 0xE3}, "EX (SP),IY", 2);
  expect(m, {0xDD, 0xF9}, "LD SP,IX", 2);
  expect(m, {0xDD, 0x09}, "ADD IX,BC", 2);
  expect(m, {0xDD, 0x29}, "ADD IX,IX", 2);
  expect(m, {0xFD, 0x22, 0x00, 0x40}, "LD (0x4000),IY", 4);
  expect(m, {0xDD, 0x2A, 0x00, 0x40}, "LD IX,(0x4000)", 4);
}

TEST(disassembler_keeps_the_last_of_repeated_prefixes) {
  Memory m;
  expect(m, {0xDD, 0xFD, 0x21, 0x34, 0x12}, "LD IY,0x1234", 5);
  expect(m, {0xFD, 0xDD, 0x7E, 0x01}, "LD A,(IX+1)", 4);
}

TEST(disassembler_handles_indexed_cb_operations) {
  Memory m;
  expect(m, {0xDD, 0xCB, 0x01, 0x00}, "RLC (IX+1),B", 4);
  expect(m, {0xDD, 0xCB, 0x01, 0x06}, "RLC (IX+1)", 4);
  expect(m, {0xDD, 0xCB, 0x01, 0x56}, "BIT 2,(IX+1)", 4);
  expect(m, {0xFD, 0xCB, 0x03, 0x86}, "RES 0,(IY+3)", 4);
  expect(m, {0xDD, 0xCB, 0xFF, 0xFF}, "SET 7,(IX-1),A", 4);
  expect(m, {0xDD, 0xCB, 0xFF, 0xFE}, "SET 7,(IX-1)", 4);
}

TEST(disassembler_handles_the_ed_prefix) {
  Memory m;
  expect(m, {0xED, 0x43, 0x34, 0x12}, "LD (0x1234),BC", 4);
  expect(m, {0xED, 0x4B, 0x34, 0x12}, "LD BC,(0x1234)", 4);
  expect(m, {0xED, 0x52}, "SBC HL,DE", 2);
  expect(m, {0xED, 0x7A}, "ADC HL,SP", 2);
  expect(m, {0xED, 0x44}, "NEG", 2);
  expect(m, {0xED, 0x5E}, "IM 2", 2);
  expect(m, {0xED, 0x47}, "LD I,A", 2);
  expect(m, {0xED, 0x57}, "LD A,I", 2);
  expect(m, {0xED, 0x67}, "RRD", 2);
  expect(m, {0xED, 0x6F}, "RLD", 2);
  expect(m, {0xED, 0xB0}, "LDIR", 2);
  expect(m, {0xED, 0xB8}, "LDDR", 2);
  expect(m, {0xED, 0x40}, "IN B,(C)", 2);
  expect(m, {0xED, 0x70}, "IN (C)", 2);
  expect(m, {0xED, 0x79}, "OUT (C),A", 2);
  expect(m, {0xED, 0x71}, "OUT (C),0", 2);
}

TEST(disassembler_treats_unknown_ed_opcodes_as_two_nops) {
  Memory m;
  expect(m, {0xED, 0x00}, "NOP", 2);
  expect(m, {0xED, 0x23}, "NOP", 2);
  expect(m, {0xED, 0x91, 0x07}, "NOP", 2);
}

TEST(disassembler_knows_the_z80n_extensions) {
  Memory m;
  expect(m, {0xED, 0x23}, "SWAPNIB", 2, Flow::Next, true);
  expect(m, {0xED, 0x24}, "MIRROR A", 2, Flow::Next, true);
  expect(m, {0xED, 0x27, 0x0F}, "TEST 0x0F", 3, Flow::Next, true);
  expect(m, {0xED, 0x28}, "BSLA DE,B", 2, Flow::Next, true);
  expect(m, {0xED, 0x2C}, "BRLC DE,B", 2, Flow::Next, true);
  expect(m, {0xED, 0x30}, "MUL D,E", 2, Flow::Next, true);
  expect(m, {0xED, 0x31}, "ADD HL,A", 2, Flow::Next, true);
  expect(m, {0xED, 0x32}, "ADD DE,A", 2, Flow::Next, true);
  expect(m, {0xED, 0x33}, "ADD BC,A", 2, Flow::Next, true);
  expect(m, {0xED, 0x34, 0x34, 0x12}, "ADD HL,0x1234", 4, Flow::Next, true);
  expect(m, {0xED, 0x35, 0x34, 0x12}, "ADD DE,0x1234", 4, Flow::Next, true);
  expect(m, {0xED, 0x36, 0x34, 0x12}, "ADD BC,0x1234", 4, Flow::Next, true);
  // PUSH nn is the one big-endian immediate in the instruction set.
  expect(m, {0xED, 0x8A, 0x12, 0x34}, "PUSH 0x1234", 4, Flow::Next, true);
  expect(m, {0xED, 0x90}, "OUTINB", 2, Flow::Next, true);
  expect(m, {0xED, 0x91, 0x07, 0x03}, "NEXTREG 0x07,0x03", 4, Flow::Next, true);
  expect(m, {0xED, 0x92, 0x07}, "NEXTREG 0x07,A", 3, Flow::Next, true);
  expect(m, {0xED, 0x93}, "PIXELDN", 2, Flow::Next, true);
  expect(m, {0xED, 0x94}, "PIXELAD", 2, Flow::Next, true);
  expect(m, {0xED, 0x95}, "SETAE", 2, Flow::Next, true);
  expect(m, {0xED, 0xA4}, "LDIX", 2, Flow::Next, true);
  expect(m, {0xED, 0xA5}, "LDWS", 2, Flow::Next, true);
  expect(m, {0xED, 0xB4}, "LDIRX", 2, Flow::Next, true);
  expect(m, {0xED, 0xB7}, "LDPIRX", 2, Flow::Next, true);
  expect(m, {0xED, 0xBC}, "LDDRX", 2, Flow::Next, true);
  expect(m, {0xED, 0x98}, "JP (C)", 2, Flow::Jump, true);
}

TEST(disassembler_computes_relative_jump_targets) {
  Memory m;
  // Both jumps land back on themselves, which is the usual idle loop.
  expect(m, {0x18, 0xFE}, "JR 0x8000", 2, Flow::Jump);
  expect(m, {0x10, 0xFE}, "DJNZ 0x8000", 2, Flow::Jump);
  expect(m, {0x18, 0x03}, "JR 0x8005", 2, Flow::Jump);
  // JR has only four conditions, so the field is two bits wide.
  expect(m, {0x20, 0x02}, "JR NZ,0x8004", 2, Flow::Jump);
  expect(m, {0x28, 0x02}, "JR Z,0x8004", 2, Flow::Jump);
  expect(m, {0x30, 0x02}, "JR NC,0x8004", 2, Flow::Jump);
  expect(m, {0x38, 0x02}, "JR C,0x8004", 2, Flow::Jump);
  // The base is the address after the instruction, not the opcode's.
  const Disassembled later = decodeAt(m, 0x9000, {0x18, 0xFC});
  CHECK_EQ(later.length, 2);
  CHECK(later.text == "JR 0x8FFE");
}

TEST(disassembler_reports_absolute_flow) {
  Memory m;
  expect(m, {0xC3, 0x00, 0x80}, "JP 0x8000", 3, Flow::Jump);
  expect(m, {0xE9}, "JP (HL)", 1, Flow::Jump);
  expect(m, {0xDD, 0xE9}, "JP (IX)", 2, Flow::Jump);
  expect(m, {0xCD, 0x00, 0x80}, "CALL 0x8000", 3, Flow::Call);
  expect(m, {0xC9}, "RET", 1, Flow::Return);
  expect(m, {0xFF}, "RST 0x38", 1, Flow::Call);
  expect(m, {0xC7}, "RST 0x00", 1, Flow::Call);
  expect(m, {0xEF}, "RST 0x28", 1, Flow::Call);
  expect(m, {0xED, 0x45}, "RETN", 2, Flow::Return);
  expect(m, {0xED, 0x4D}, "RETI", 2, Flow::Return);
}

TEST(disassembler_marks_conditional_flow_too) {
  Memory m;
  expect(m, {0xC2, 0x00, 0x80}, "JP NZ,0x8000", 3, Flow::Jump);
  expect(m, {0xDA, 0x00, 0x80}, "JP C,0x8000", 3, Flow::Jump);
  expect(m, {0xC4, 0x00, 0x80}, "CALL NZ,0x8000", 3, Flow::Call);
  expect(m, {0xF4, 0x00, 0x80}, "CALL P,0x8000", 3, Flow::Call);
  expect(m, {0xC0}, "RET NZ", 1, Flow::Return);
  expect(m, {0xF8}, "RET M", 1, Flow::Return);
}

TEST(disassembler_handles_the_remaining_base_opcodes) {
  Memory m;
  expect(m, {0x00}, "NOP", 1);
  expect(m, {0x02}, "LD (BC),A", 1);
  expect(m, {0x0A}, "LD A,(BC)", 1);
  expect(m, {0x12}, "LD (DE),A", 1);
  expect(m, {0x1A}, "LD A,(DE)", 1);
  expect(m, {0x07}, "RLCA", 1);
  expect(m, {0x0F}, "RRCA", 1);
  expect(m, {0x17}, "RLA", 1);
  expect(m, {0x1F}, "RRA", 1);
  expect(m, {0x27}, "DAA", 1);
  expect(m, {0x2F}, "CPL", 1);
  expect(m, {0x37}, "SCF", 1);
  expect(m, {0x3F}, "CCF", 1);
  expect(m, {0x08}, "EX AF,AF'", 1);
  expect(m, {0xD9}, "EXX", 1);
  expect(m, {0xEB}, "EX DE,HL", 1);
  expect(m, {0xF3}, "DI", 1);
  expect(m, {0xFB}, "EI", 1);
  expect(m, {0x32, 0x00, 0x40}, "LD (0x4000),A", 3);
  expect(m, {0x3A, 0x00, 0x40}, "LD A,(0x4000)", 3);
  expect(m, {0xD3, 0xFE}, "OUT (0xFE),A", 2);
  expect(m, {0xDB, 0xFE}, "IN A,(0xFE)", 2);
  expect(m, {0x03}, "INC BC", 1);
  expect(m, {0x0B}, "DEC BC", 1);
  expect(m, {0x33}, "INC SP", 1);
  expect(m, {0x3B}, "DEC SP", 1);
  expect(m, {0xC5}, "PUSH BC", 1);
  expect(m, {0xF1}, "POP AF", 1);
  expect(m, {0xF5}, "PUSH AF", 1);
}

TEST(disassembler_reads_through_the_reader_for_each_address) {
  Memory m;
  // Two different instructions at two addresses prove nothing is cached.
  const Disassembled first = decodeAt(m, 0xC000, {0x3E, 0x01});
  const Disassembled second = decodeAt(m, 0x4000, {0xC9});
  CHECK(first.text == "LD A,0x01");
  CHECK_EQ(first.length, 2);
  CHECK(second.text == "RET");
  CHECK_EQ(second.length, 1);
}
