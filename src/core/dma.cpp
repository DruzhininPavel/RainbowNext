#include "core/dma.h"

namespace mynext {

void ZxnDma::reset() {
  aStart_ = 0;
  bStart_ = 0;
  length_ = 0;
  directionAToB_ = false;
  aIsIo_ = false;
  bIsIo_ = false;
  aMode_ = 1;
  bMode_ = 1;
  cycleA_ = 0;
  cycleB_ = 0;
  prescalar_ = 0;
  mode_ = 1;
  autoRestart_ = false;
  legacy_ = false;

  enabled_ = false;
  busy_ = false;
  aPtr_ = 0;
  bPtr_ = 0;
  counter_ = 0;
  accum_ = 0;
  endOfBlock_ = true;
  transferred_ = false;

  readMask_ = 0x7F;
  readIndex_ = 0;

  params_.fill(Param::None);
  paramCount_ = 0;
  paramIndex_ = 0;
  expectMask_ = false;
}

void ZxnDma::connect(ReadPort read, WritePort write) {
  readPort_ = std::move(read);
  writePort_ = std::move(write);
}

void ZxnDma::write(u8 value, bool zilogMode) {
  // A transfer is running on the bus, so it cannot also be programming the
  // device.
  if (busy_) return;
  legacy_ = zilogMode;

  if (paramIndex_ < paramCount_) {
    applyParam(params_[size_t(paramIndex_++)], value);
    return;
  }
  if (expectMask_) {
    expectMask_ = false;
    readMask_ = value;
    readIndex_ = 0;
    return;
  }
  decodeRegister(value);
}

void ZxnDma::decodeRegister(u8 value) {
  // A new register group starts its own list of parameter bytes.
  paramCount_ = 0;
  paramIndex_ = 0;

  if ((value & 0x80) == 0) {
    // The low two bits of a WR0 byte must not both be zero, which is what keeps
    // it apart from WR1 (100) and WR2 (000).
    switch (value & 0x07) {
      case 0x04: // WR1: port A configuration
        aMode_ = u8((value >> 4) & 0x03);
        aIsIo_ = (value & 0x08) != 0;
        // D6 asks for the variable timing byte, as on the Z80 DMA.
        if (value & 0x40) push(Param::Wr1Timing);
        return;
      case 0x00: // WR2: port B configuration
        bMode_ = u8((value >> 4) & 0x03);
        bIsIo_ = (value & 0x08) != 0;
        if (value & 0x40) push(Param::Wr2Timing);
        return;
      default: { // WR0: direction, operation, port A start, block length
        // D2 picks the direction; D1:D0 must be the transfer operation, never
        // 00, which is what keeps this byte apart from WR1 and WR2.
        directionAToB_ = (value & 0x04) != 0;
        if (value & 0x08) push(Param::Wr0AStartLow);
        if (value & 0x10) push(Param::Wr0AStartHigh);
        if (value & 0x20) push(Param::Wr0LengthLow);
        if (value & 0x40) push(Param::Wr0LengthHigh);
        return;
      }
    }
  }

  switch (value & 0x03) {
    case 0x00: // WR3: enable
      if (value & 0x40) {
        enable();
      } else {
        enabled_ = false;
      }
      return;
    case 0x01: // WR4: mode and port B start
      mode_ = u8((value >> 5) & 0x03);
      // D2 and D3 ask for the low and the high byte of port B, low first.
      if (value & 0x04) push(Param::Wr4BStartLow);
      if (value & 0x08) push(Param::Wr4BStartHigh);
      return;
    case 0x02: // WR5: ready and stop configuration
      autoRestart_ = (value & 0x08) != 0;
      return;
    default: // WR6: command
      command(value);
      return;
  }
}

void ZxnDma::applyParam(Param param, u8 value) {
  switch (param) {
    case Param::Wr0AStartLow:
      aStart_ = u16((aStart_ & 0xFF00) | value);
      return;
    case Param::Wr0AStartHigh:
      aStart_ = u16((aStart_ & 0x00FF) | (u16(value) << 8));
      return;
    case Param::Wr0LengthLow:
      length_ = u16((length_ & 0xFF00) | value);
      return;
    case Param::Wr0LengthHigh:
      length_ = u16((length_ & 0x00FF) | (u16(value) << 8));
      return;
    case Param::Wr1Timing:
      cycleA_ = u8(value & 0x03);
      return;
    case Param::Wr2Timing:
      cycleB_ = u8(value & 0x03);
      // On the Z80 DMA this bit asks for a match mask byte; the zxnDMA has no
      // search hardware and puts its prescalar in that slot instead.
      if (value & 0x20) push(Param::Wr2Prescalar);
      return;
    case Param::Wr2Prescalar:
      prescalar_ = value;
      return;
    case Param::Wr4BStartLow:
      bStart_ = u16((bStart_ & 0xFF00) | value);
      return;
    case Param::Wr4BStartHigh:
      bStart_ = u16((bStart_ & 0x00FF) | (u16(value) << 8));
      return;
    case Param::None:
      return;
  }
}

void ZxnDma::command(u8 value) {
  switch ((value >> 2) & 0x3F) {
    case 0x30: // 0xC3 reset
      reset();
      return;
    case 0x31: // 0xC7 reset port A timing
      cycleA_ = 0;
      prescalar_ = 0;
      return;
    case 0x32: // 0xCB reset port B timing
      cycleB_ = 0;
      prescalar_ = 0;
      return;
    case 0x2F: // 0xBF read status byte
      readMask_ = 0x01;
      readIndex_ = 0;
      return;
    case 0x22: // 0x8B reinitialize status byte
      endOfBlock_ = true;
      transferred_ = false;
      return;
    case 0x29: // 0xA7 initialize read sequence
      readIndex_ = 0;
      return;
    case 0x33: // 0xCF load
      load();
      return;
    case 0x34: // 0xD3 continue
      counter_ = blockLength();
      return;
    case 0x21: // 0x87 enable
      enable();
      return;
    case 0x20: // 0x83 disable
      enabled_ = false;
      return;
    case 0x2E: // 0xBB read mask follows
      expectMask_ = true;
      return;
    default:
      // Force ready and the interrupt commands do nothing here.
      return;
  }
}

void ZxnDma::load() {
  aPtr_ = aStart_;
  bPtr_ = bStart_;
  counter_ = blockLength();
}

void ZxnDma::enable() {
  enabled_ = true;
  if (counter_ == 0) {
    enabled_ = false;
    return;
  }
  if (prescalar_ == 0) {
    // Without a prescalar the block is done before the CPU runs its next
    // instruction. A block that auto restarts would never end, so it is cut
    // off after one pass over the 64K address space.
    int budget = 0x10000;
    while (counter_ > 0 && budget-- > 0) transferByte();
    if (counter_ > 0) enabled_ = false;
    return;
  }
  // The first byte goes out at once; the rest follow one prescalar apart.
  accum_ = 0;
  transferByte();
}

void ZxnDma::advance(u64 masterTicks) {
  if (!running() || prescalar_ == 0) return;
  const u64 period = u64(prescalar_) * TicksPerPrescalarUnit;
  accum_ += masterTicks;
  while (running() && accum_ >= period) {
    accum_ -= period;
    transferByte();
  }
}

void ZxnDma::advancePointer(u16& address, u8 mode) const {
  switch (mode & 0x03) {
    case 0x00:
      address = u16(address - 1);
      return;
    case 0x01:
      address = u16(address + 1);
      return;
    default:
      return; // 1x leaves the address alone
  }
}

void ZxnDma::transferByte() {
  if (counter_ == 0) return;

  const u16 source = sourceAddress();
  const u16 dest = destAddress();
  busy_ = true;
  const u8 value = readPort_ ? readPort_(sourceIsIo(), source) : u8(0xFF);
  if (writePort_) writePort_(destIsIo(), dest, value);
  busy_ = false;

  if (directionAToB_) {
    advancePointer(aPtr_, aMode_);
    advancePointer(bPtr_, bMode_);
  } else {
    advancePointer(bPtr_, bMode_);
    advancePointer(aPtr_, aMode_);
  }

  --counter_;
  transferred_ = true;
  if (counter_ > 0) return;

  endOfBlock_ = false;
  if (autoRestart_) {
    aPtr_ = aStart_;
    bPtr_ = bStart_;
    counter_ = blockLength();
  }
  if (counter_ == 0) enabled_ = false;
}

u8 ZxnDma::read(bool zilogMode) {
  legacy_ = zilogMode;
  const std::array<u8, 7> values = {
      status(),
      u8(counter_ & 0xFF),
      u8(counter_ >> 8),
      u8(aPtr_ & 0xFF),
      u8(aPtr_ >> 8),
      u8(bPtr_ & 0xFF),
      u8(bPtr_ >> 8),
  };
  // The read mask picks which of the seven are driven; the rest are skipped and
  // the sequence wraps round to the first one that is left.
  for (int step = 0; step < int(values.size()); ++step) {
    const int position = readIndex_;
    readIndex_ = (readIndex_ + 1) % int(values.size());
    if ((readMask_ >> position) & 1) return values[size_t(position)];
  }
  return 0;
}

} // namespace mynext
