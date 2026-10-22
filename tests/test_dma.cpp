// The zxnDMA.
//
// The device is programmed through one port with a sequence of register groups:
// the first byte of a group selects the register and says which parameter bytes
// follow, and the parameters arrive from the lowest flag bit upwards. A LOAD
// command copies the programmed start addresses into the working pointers, and
// an ENABLE command starts the block. With no prescalar the whole block is done
// before the CPU runs its next instruction; with a prescalar the bytes go out
// one at a time as the clock advances.

#include <array>
#include <vector>

#include "test_framework.h"

#include "core/dma.h"
#include "core/machine.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::ZxnDma;
using mynext::u16;
using mynext::u32;
using mynext::u8;

namespace {

constexpr u8 Wr0AToB = 0x7D; // transfer, port A -> port B, both addresses
constexpr u8 Wr0BToA = 0x79; // transfer, port B -> port A

// Increment, decrement and fixed, the three address modes.
constexpr u8 Inc = 1;
constexpr u8 Dec = 0;
constexpr u8 Fixed = 2;

// A device with a flat 64K of memory and 64K of IO behind it.
struct Bus {
  std::array<u8, 0x10000> mem{};
  std::array<u8, 0x10000> io{};
  int ioReads = 0;
  int ioWrites = 0;
  ZxnDma dma;

  Bus() {
    dma.connect(
        [this](bool isIo, u16 address) -> u8 {
          if (isIo) {
            ++ioReads;
            return io[address];
          }
          return mem[address];
        },
        [this](bool isIo, u16 address, u8 value) {
          if (isIo) {
            ++ioWrites;
            io[address] = value;
          } else {
            mem[address] = value;
          }
        });
  }

  void out(u8 value) { dma.write(value, false); }
};

void setPortA(ZxnDma& dma, u8 mode, bool isIo) {
  dma.write(u8(0x44 | u8((mode & 0x03) << 4) | (isIo ? 0x08 : 0x00)));
  dma.write(0x02); // cycle length 2
}

void setPortB(ZxnDma& dma, u8 mode, bool isIo, u8 prescalar = 0) {
  dma.write(u8(0x40 | u8((mode & 0x03) << 4) | (isIo ? 0x08 : 0x00)));
  if (prescalar == 0) {
    dma.write(0x02);
    return;
  }
  // D5 of the timing byte asks for the ZXN prescalar to follow.
  dma.write(0x22);
  dma.write(prescalar);
}

// Programs a whole block the way the wiki's example code does, stopping short
// of the LOAD so a test can look at the programmed registers first.
void programBlock(ZxnDma& dma, u16 a, u16 b, u16 length, bool aToB = true) {
  dma.write(0x83); // disable
  dma.write(aToB ? Wr0AToB : Wr0BToA);
  dma.write(u8(a & 0xFF));
  dma.write(u8(a >> 8));
  dma.write(u8(length & 0xFF));
  dma.write(u8(length >> 8));
  setPortA(dma, Inc, false);
  setPortB(dma, Inc, false);
  dma.write(0xAD); // continuous mode, both port B bytes follow
  dma.write(u8(b & 0xFF));
  dma.write(u8(b >> 8));
  dma.write(0x82); // stop at the end of the block
}

void start(ZxnDma& dma) {
  dma.write(0xCF); // load
  dma.write(0x87); // enable
}

void fill(Bus& bus, u16 at, const std::vector<u8>& bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) bus.mem[u16(at + i)] = bytes[i];
}

} // namespace

// ---------------------------------------------------------------------------
// Block transfers
// ---------------------------------------------------------------------------

TEST(dma_copies_a_block_from_port_a_to_port_b) {
  Bus bus;
  fill(bus, 0x8000, {0x10, 0x20, 0x30, 0x40});
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  start(bus.dma);

  CHECK_EQ(bus.mem[0xC000], 0x10);
  CHECK_EQ(bus.mem[0xC001], 0x20);
  CHECK_EQ(bus.mem[0xC002], 0x30);
  CHECK_EQ(bus.mem[0xC003], 0x40);
  CHECK_EQ(bus.dma.bytesRemaining(), 0);
  CHECK(!bus.dma.running());
}

TEST(dma_copies_a_block_from_port_b_to_port_a) {
  Bus bus;
  fill(bus, 0x8000, {0xAA, 0xBB});
  programBlock(bus.dma, 0x9000, 0x8000, 2, false);
  start(bus.dma);

  CHECK_EQ(bus.mem[0x9000], 0xAA);
  CHECK_EQ(bus.mem[0x9001], 0xBB);
}

// In zxnDMA mode the programmed length is the number of bytes moved; it is the
// Zilog port that adds the extra byte.
TEST(dma_moves_exactly_the_programmed_number_of_bytes) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4, 5, 6, 7, 8});
  bus.mem[0xC004] = 0xEE;
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  start(bus.dma);

  CHECK_EQ(bus.mem[0xC003], 4);
  CHECK_EQ(bus.mem[0xC004], 0xEE); // untouched
}

TEST(dma_moves_one_extra_byte_through_the_zilog_port) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4, 5});
  bus.dma.write(0x83, true);
  bus.dma.write(Wr0AToB, true);
  bus.dma.write(0x00, true);
  bus.dma.write(0x80, true);
  bus.dma.write(0x04, true); // length 4, which the Zilog port makes five
  bus.dma.write(0x00, true);
  bus.dma.write(0x54, true);
  bus.dma.write(0x02, true);
  bus.dma.write(0x50, true);
  bus.dma.write(0x02, true);
  bus.dma.write(0xAD, true);
  bus.dma.write(0x00, true);
  bus.dma.write(0xC0, true);
  bus.dma.write(0x82, true);
  bus.dma.write(0xCF, true);
  bus.dma.write(0x87, true);

  CHECK_EQ(bus.mem[0xC003], 4);
  CHECK_EQ(bus.mem[0xC004], 5);
  CHECK_EQ(bus.mem[0xC005], 0);
}

TEST(dma_without_a_load_does_not_move_anything) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4});
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  bus.out(0x87); // enable, but no LOAD came first

  CHECK_EQ(bus.mem[0xC000], 0);
  CHECK(!bus.dma.running());
}

TEST(dma_load_copies_the_start_addresses_into_the_pointers) {
  Bus bus;
  programBlock(bus.dma, 0x8123, 0xC456, 0x0010);
  bus.out(0xCF);

  CHECK_EQ(bus.dma.portAAddress(), 0x8123);
  CHECK_EQ(bus.dma.portBAddress(), 0xC456);
  CHECK_EQ(bus.dma.bytesRemaining(), 0x10);
}

TEST(dma_can_hold_a_port_at_one_address) {
  Bus bus;
  bus.mem[0x8000] = 0x5A;
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  setPortA(bus.dma, Fixed, false);
  start(bus.dma);

  for (u16 i = 0; i < 4; ++i) CHECK_EQ(bus.mem[0xC000 + i], 0x5A);
}

TEST(dma_can_walk_a_port_downwards) {
  Bus bus;
  bus.mem[0x8010] = 0x11;
  bus.mem[0x800F] = 0x22;
  bus.mem[0x800E] = 0x33;
  programBlock(bus.dma, 0x8010, 0xC000, 3);
  setPortA(bus.dma, Dec, false);
  start(bus.dma);

  CHECK_EQ(bus.mem[0xC000], 0x11);
  CHECK_EQ(bus.mem[0xC001], 0x22);
  CHECK_EQ(bus.mem[0xC002], 0x33);
}

TEST(dma_reaches_io_through_port_b) {
  Bus bus;
  fill(bus, 0x8000, {0x12, 0x34});
  programBlock(bus.dma, 0x8000, 0x253B, 2);
  setPortB(bus.dma, Inc, true);
  start(bus.dma);

  CHECK_EQ(bus.io[0x253B], 0x12);
  CHECK_EQ(bus.io[0x253C], 0x34);
  CHECK_EQ(bus.ioWrites, 2);
}

TEST(dma_reaches_io_through_port_a) {
  Bus bus;
  bus.io[0x1000] = 0x77;
  programBlock(bus.dma, 0x1000, 0xC000, 1);
  setPortA(bus.dma, Fixed, true);
  start(bus.dma);

  CHECK_EQ(bus.mem[0xC000], 0x77);
  CHECK_EQ(bus.ioReads, 1);
}

TEST(dma_continue_repeats_from_the_current_pointers) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4, 5, 6});
  programBlock(bus.dma, 0x8000, 0xC000, 2);
  start(bus.dma);
  CHECK_EQ(bus.mem[0xC000], 1);
  CHECK_EQ(bus.mem[0xC001], 2);

  bus.out(0xD3); // continue: the pointers stay where the last block left them
  bus.out(0x87);
  CHECK_EQ(bus.mem[0xC002], 3);
  CHECK_EQ(bus.mem[0xC003], 4);
}

TEST(dma_auto_restart_repeats_the_block) {
  Bus bus;
  fill(bus, 0x8000, {0x11, 0x22});
  programBlock(bus.dma, 0x8000, 0xC000, 2);
  setPortB(bus.dma, Inc, false, 2); // a prescalar, so the block is spread out
  bus.out(0x8A);                   // auto restart at the end of the block
  start(bus.dma);

  const int period = ZxnDma::TicksPerPrescalarUnit * 2;
  bus.dma.advance(u16(period)); // the second byte, ending the first pass
  CHECK_EQ(bus.mem[0xC000], 0x11);
  CHECK_EQ(bus.mem[0xC001], 0x22);

  // The block restarted: the pointers are back at the start and the counter is
  // full again, so the next byte lands on the first address once more.
  CHECK(bus.dma.running());
  CHECK_EQ(bus.dma.portAAddress(), 0x8000);
  CHECK_EQ(bus.dma.bytesRemaining(), 2);
  bus.dma.advance(u16(period));
  CHECK_EQ(bus.dma.bytesRemaining(), 1);
  CHECK_EQ(bus.dma.portAAddress(), 0x8001);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

TEST(dma_without_a_prescalar_finishes_inside_the_enable_write) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4});
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  start(bus.dma);

  CHECK(!bus.dma.timed());
  CHECK_EQ(bus.mem[0xC003], 4);
}

TEST(dma_with_a_prescalar_moves_one_byte_per_period) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4});
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  setPortB(bus.dma, Inc, false, 4);
  start(bus.dma);

  CHECK(bus.dma.timed());
  CHECK_EQ(bus.dma.prescalar(), 4);
  CHECK_EQ(bus.dma.bytesRemaining(), 3); // the first byte went out at once
  CHECK_EQ(bus.mem[0xC000], 1);

  const int period = ZxnDma::TicksPerPrescalarUnit * 4;
  bus.dma.advance(u16(period - 1));
  CHECK_EQ(bus.dma.bytesRemaining(), 3);
  bus.dma.advance(1);
  CHECK_EQ(bus.dma.bytesRemaining(), 2);

  bus.dma.advance(u16(period * 2));
  CHECK_EQ(bus.dma.bytesRemaining(), 0);
  CHECK_EQ(bus.mem[0xC003], 4);
  CHECK(!bus.dma.running());
}

TEST(dma_advance_does_nothing_without_a_prescalar) {
  Bus bus;
  fill(bus, 0x8000, {1, 2});
  programBlock(bus.dma, 0x8000, 0xC000, 2);
  start(bus.dma);

  bus.dma.advance(100000);
  CHECK_EQ(bus.mem[0xC001], 2);
}

TEST(dma_disable_stops_a_timed_transfer) {
  Bus bus;
  fill(bus, 0x8000, {1, 2, 3, 4});
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  setPortB(bus.dma, Inc, false, 4);
  start(bus.dma);

  bus.out(0x83);
  bus.dma.advance(100000);
  CHECK_EQ(bus.dma.bytesRemaining(), 3);
  CHECK(!bus.dma.running());
}

TEST(dma_reset_clears_the_prescalar) {
  Bus bus;
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  setPortB(bus.dma, Inc, false, 9);
  CHECK_EQ(bus.dma.prescalar(), 9);

  bus.out(0xC7); // reset port A timing
  CHECK_EQ(bus.dma.prescalar(), 0);

  setPortB(bus.dma, Inc, false, 9);
  bus.out(0xCB); // reset port B timing
  CHECK_EQ(bus.dma.prescalar(), 0);

  setPortB(bus.dma, Inc, false, 9);
  bus.out(0xC3); // reset everything
  CHECK_EQ(bus.dma.prescalar(), 0);
  CHECK_EQ(bus.dma.readMask(), 0x7F);
  CHECK_EQ(bus.dma.bytesRemaining(), 0);
}

// ---------------------------------------------------------------------------
// Status and the read sequence
// ---------------------------------------------------------------------------

TEST(dma_status_byte_reports_progress) {
  Bus bus;
  fill(bus, 0x8000, {1, 2});
  programBlock(bus.dma, 0x8000, 0xC000, 2);
  // Nothing has been transferred: E is clear, T is clear.
  CHECK_EQ(bus.dma.status() & 0x21, 0x20);

  start(bus.dma);
  // The block completed: E goes low and T goes high.
  CHECK_EQ(bus.dma.status() & 0x21, 0x01);
}

TEST(dma_read_sequence_follows_the_mask) {
  Bus bus;
  programBlock(bus.dma, 0x8123, 0xC456, 0x0010);
  bus.out(0xCF);

  // The power-on mask is 0x7F, so all seven bytes are driven in order.
  CHECK_EQ(bus.dma.read(false), bus.dma.status());
  CHECK_EQ(bus.dma.read(false), 0x10); // byte counter low
  CHECK_EQ(bus.dma.read(false), 0x00); // byte counter high
  CHECK_EQ(bus.dma.read(false), 0x23); // port A low
  CHECK_EQ(bus.dma.read(false), 0x81); // port A high
  CHECK_EQ(bus.dma.read(false), 0x56); // port B low
  CHECK_EQ(bus.dma.read(false), 0xC4); // port B high
  // The sequence wraps round to the status byte.
  CHECK_EQ(bus.dma.read(false), bus.dma.status());
}

TEST(dma_read_mask_selects_which_bytes_are_driven) {
  Bus bus;
  programBlock(bus.dma, 0x8123, 0xC456, 0x0010);
  bus.out(0xCF);
  bus.out(0xBB); // read mask follows
  bus.out(0x18); // the two port A address bytes only

  CHECK_EQ(bus.dma.readMask(), 0x18);
  CHECK_EQ(bus.dma.read(false), 0x23);
  CHECK_EQ(bus.dma.read(false), 0x81);
  CHECK_EQ(bus.dma.read(false), 0x23); // wrapped
}

TEST(dma_read_status_command_narrows_the_mask) {
  Bus bus;
  programBlock(bus.dma, 0x8000, 0xC000, 4);
  bus.out(0xBF); // read status byte
  CHECK_EQ(bus.dma.readMask(), 0x01);
  CHECK_EQ(bus.dma.read(false), bus.dma.status());
  CHECK_EQ(bus.dma.read(false), bus.dma.status());
}

// ---------------------------------------------------------------------------
// The device inside the machine
// ---------------------------------------------------------------------------

TEST(dma_copies_memory_through_the_machine_port) {
  Machine m(MachineType::Next);
  for (u16 i = 0; i < 8; ++i) m.memWrite(u16(0x8000 + i), u8(0x40 + i), 0);

  const std::vector<u8> program = {
      0x83,             // disable
      0x7D, 0x00, 0x80, // port A = 0x8000
      0x08, 0x00,       // eight bytes
      0x54, 0x02,       // port A: increment, memory, timing byte
      0x50, 0x02,       // port B: increment, memory, timing byte
      0xAD, 0x00, 0xC0, // continuous mode, port B = 0xC000
      0x82,             // stop at the end of the block
      0xCF,             // load
      0x87,             // enable
  };
  for (u8 value : program) m.ioWrite(0x6B, value, 0);

  for (u16 i = 0; i < 8; ++i) CHECK_EQ(m.memRead(u16(0xC000 + i), 0), 0x40 + i);
  CHECK_EQ(m.dma().bytesRemaining(), 0);
}

TEST(dma_writes_a_next_register_through_port_0x253b) {
  Machine m(MachineType::Next);
  m.memWrite(0x8000, 0x07, 0);
  m.ioWrite(0x243B, 0x40, 0); // select the palette index register

  const std::vector<u8> program = {
      0x83,
      0x7D, 0x00, 0x80, // port A = 0x8000
      0x01, 0x00,       // one byte
      0x54, 0x02,       // port A: increment, memory
      0x58, 0x02,       // port B: increment, IO
      0xAD, 0x3B, 0x25, // continuous mode, port B = 0x253B
      0x82,
      0xCF,
      0x87,
  };
  for (u8 value : program) m.ioWrite(0x6B, value, 0);

  CHECK_EQ(m.readNextReg(0x40), 0x07);
}

TEST(dma_advances_with_the_machine_clock) {
  Machine m(MachineType::Next);
  for (u16 i = 0; i < 4; ++i) m.memWrite(u16(0x8000 + i), u8(0x50 + i), 0);

  const std::vector<u8> program = {
      0x83,
      0x7D, 0x00, 0x80,
      0x04, 0x00,
      0x54, 0x02,
      0x50, 0x22, 0x04, // port B: increment, memory, prescalar 4
      0xAD, 0x00, 0xC0,
      0x82,
      0xCF,
      0x87,
  };
  for (u8 value : program) m.ioWrite(0x6B, value, 0);

  CHECK_EQ(m.memRead(0xC000, 0), 0x50);
  CHECK_EQ(m.dma().bytesRemaining(), 3);

  // One prescalar period of 28 MHz clocks, and the next byte is out.
  const u32 period = u32(ZxnDma::TicksPerPrescalarUnit) * 4;
  m.runCycles(period / 8);
  CHECK_EQ(m.dma().bytesRemaining(), 2);
  CHECK_EQ(m.memRead(0xC001, 0), 0x51);
}
