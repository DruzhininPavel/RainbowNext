#pragma once

#include <array>
#include <functional>

#include "core/types.h"

namespace rainbow {

// The Next's zxnDMA: a single channel of the Z80 DMA mapped to port 0x6B, with
// port 0x0B as the Zilog compatibility port, where a block is one byte longer
// than the programmed length.
//
// The device is programmed by writing a sequence of "register groups" to the
// port. The first byte of a group says which register it is; the bits the group
// leaves free are flags for the parameter bytes that follow, delivered from the
// lowest flag bit upwards. The register file is:
//
//   WR0  direction, operation, port A start and block length
//   WR1  port A: address mode and memory/IO
//   WR2  port B: address mode and memory/IO, plus the ZXN prescalar
//   WR3  enable
//   WR4  continuous or burst mode, port B start
//   WR5  stop or auto restart at the end of a block
//   WR6  commands: reset, load, continue, enable, disable, read status
//
// A transfer is a byte read from one port and written to the other. With a zero
// prescalar the whole block is done as soon as the DMA is enabled; with a
// non-zero prescalar each byte is due 32*prescalar master clocks after the
// previous one, which is how the DMA feeds a sample rate to the DAC without
// stopping the CPU.
//
// The bus is injected as two callbacks so the device stays independent of the
// machine: `read(isIo, address)` and `write(isIo, address, value)`.
class ZxnDma {
public:
  using ReadPort = std::function<u8(bool isIo, u16 address)>;
  using WritePort = std::function<void(bool isIo, u16 address, u8 value)>;

  // Master clock ticks (28 MHz) one byte takes per unit of prescalar. The wiki
  // gives the rate as 875 kHz / prescalar, and 28 MHz / 875 kHz is 32.
  static constexpr int TicksPerPrescalarUnit = 32;

  ZxnDma() { reset(); }

  // Clears the device but keeps the bus callbacks, so a machine reset does not
  // have to reconnect.
  void reset();
  void connect(ReadPort read, WritePort write);

  void write(u8 value, bool zilogMode = false);
  u8 read(bool zilogMode = false);

  bool running() const { return enabled_ && counter_ > 0; }
  bool timed() const { return prescalar_ != 0; }

  // Runs the timed transfer forward. A zero prescalar means there is nothing to
  // do: the block was already copied when the DMA was enabled.
  void advance(u64 masterTicks);

  // `00E1101T`: bit 5 is clear once a block has been completed at least once,
  // bit 0 is set once at least one byte has been moved.
  u8 status() const {
    return u8((endOfBlock_ ? 0x20 : 0x00) | 0x1A | (transferred_ ? 0x01 : 0x00));
  }

  u8 mode() const { return mode_; }
  u8 prescalar() const { return prescalar_; }
  u8 readMask() const { return readMask_; }
  u16 portAAddress() const { return aPtr_; }
  u16 portBAddress() const { return bPtr_; }
  int bytesRemaining() const { return int(counter_); }

private:
  // The parameter slot the next write will land in.
  enum class Param : u8 {
    None,
    Wr0AStartLow,
    Wr0AStartHigh,
    Wr0LengthLow,
    Wr0LengthHigh,
    Wr1Timing,
    Wr2Timing,
    Wr2Prescalar,
    Wr4BStartLow,
    Wr4BStartHigh,
  };

  void push(Param param) {
    if (paramCount_ < int(params_.size())) params_[size_t(paramCount_++)] = param;
  }
  void decodeRegister(u8 value);
  void applyParam(Param param, u8 value);
  void command(u8 value);

  void load();
  void enable();
  u32 blockLength() const { return u32(length_) + (legacy_ ? 1u : 0u); }
  void transferByte();
  bool sourceIsIo() const { return directionAToB_ ? aIsIo_ : bIsIo_; }
  bool destIsIo() const { return directionAToB_ ? bIsIo_ : aIsIo_; }
  u16 sourceAddress() const { return directionAToB_ ? aPtr_ : bPtr_; }
  u16 destAddress() const { return directionAToB_ ? bPtr_ : aPtr_; }
  void advancePointer(u16& address, u8 mode) const;

  ReadPort readPort_;
  WritePort writePort_;

  // Programmed registers.
  u16 aStart_ = 0;
  u16 bStart_ = 0;
  u16 length_ = 0;
  bool directionAToB_ = false;
  bool aIsIo_ = false;
  bool bIsIo_ = false;
  u8 aMode_ = 1; // 00 decrement, 01 increment, 1x fixed
  u8 bMode_ = 1;
  u8 cycleA_ = 0;
  u8 cycleB_ = 0;
  u8 prescalar_ = 0;
  u8 mode_ = 1; // 1 continuous, 2 burst
  bool autoRestart_ = false;
  bool legacy_ = false;

  // The transfer in progress.
  bool enabled_ = false;
  bool busy_ = false;
  u16 aPtr_ = 0;
  u16 bPtr_ = 0;
  u32 counter_ = 0;
  u64 accum_ = 0;
  bool endOfBlock_ = true;
  bool transferred_ = false;

  // The read sequence and its mask.
  u8 readMask_ = 0x7F;
  int readIndex_ = 0;

  // The parameter state machine.
  std::array<Param, 4> params_{};
  int paramCount_ = 0;
  int paramIndex_ = 0;
  bool expectMask_ = false;
};

} // namespace rainbow
