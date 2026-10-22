#include "core/sdcard.h"

#include <algorithm>

namespace rainbow {

namespace {

// A template for the card's CSD, taken from the shape a real 4 GB SDHC reports:
// CSD version 2.0, 512-byte blocks, SDHC addressing. Only C_SIZE is rewritten
// to match the image, because that is the one field a driver sanity-checks.
constexpr u8 kCsdTemplate[16] = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00,
                                 0x1D, 0x3A, 0x7F, 0x80, 0x0A, 0x40, 0x00, 0x01};

// Card identification: manufacturer 0x03, OEM "SD", the five-character product
// name "RNEXT", and a serial. Nothing reads it beyond printing it, but a driver
// that asks for it should not get garbage.
constexpr u8 kCid[16] = {0x03, 'S', 'D', 'R', 'N', 'E', 'X', 'T',
                         0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01};

// Commands the card answers. Everything else gets an illegal-command response,
// which is what a real card does and what tells a driver to stop.
enum : u8 {
  CmdGoIdle = 0,
  CmdSendOpCond = 1,
  CmdSendIfCond = 8,
  CmdSendCsd = 9,
  CmdSendCid = 10,
  CmdStopTransmission = 12,
  CmdSendStatus = 13,
  CmdSetBlockLen = 16,
  CmdReadSingle = 17,
  CmdReadMultiple = 18,
  CmdSetBlockCount = 23,
  CmdWriteBlock = 24,
  CmdWriteMultiple = 25,
  CmdSendOpCondA = 41,  // ACMD41, only valid after CMD55
  CmdAppCmd = 55,
  CmdReadOcr = 58,
  CmdCrcOnOff = 59,
};

constexpr u8 R1Idle = 0x01;
constexpr u8 R1Ready = 0x00;
constexpr u8 R1Illegal = 0x04;
constexpr u8 R1ParameterError = 0x40;
constexpr u8 DataToken = 0xFE;
constexpr u8 StopToken = 0xFD;
constexpr u8 DataAccepted = 0x05;

} // namespace

bool SdCard::load(std::vector<u8> image, std::string& error) {
  if (image.empty()) {
    error = "the card image is empty";
    return false;
  }
  if (image.size() % 512 != 0) {
    error = "the card image is not a whole number of 512-byte sectors";
    return false;
  }
  image_ = std::move(image);
  reset();
  return true;
}

void SdCard::eject() {
  image_.clear();
  reset();
}

void SdCard::reset() {
  selected_ = false;
  state_ = State::Command;
  nextState_ = State::Command;
  cmd_ = 0;
  arg_ = 0;
  frameBytes_ = 0;
  appCmd_ = false;
  idle_ = true;
  sdhc_ = false;
  out_.clear();
  outPos_ = 0;
  streamRead_ = false;
  streamAddr_ = 0;
  multiWrite_ = false;
  tokenSeen_ = false;
  writeAddr_ = 0;
  writeBuf_.clear();
}

void SdCard::select(bool asserted) {
  // Dropping chip select abandons whatever was in flight, exactly like a real
  // card: a driver that gives up mid-transfer must not leave it half-written.
  if (selected_ && !asserted) {
    state_ = State::Command;
    nextState_ = State::Command;
    frameBytes_ = 0;
    tokenSeen_ = false;
    writeBuf_.clear();
    out_.clear();
    outPos_ = 0;
    streamRead_ = false;
    multiWrite_ = false;
  }
  selected_ = asserted;
}

void SdCard::queue(const std::vector<u8>& bytes) {
  out_ = bytes;
  outPos_ = 0;
}

void SdCard::queueBlock(u32 addr) {
  out_.push_back(DataToken);
  out_.insert(out_.end(), image_.begin() + addr, image_.begin() + addr + 512);
  out_.push_back(0xFF);
  out_.push_back(0xFF);
}

u8 SdCard::transfer(u8 mosi) {
  if (!selected_ || image_.empty()) return 0xFF;

  switch (state_) {
    case State::Command: {
      if (frameBytes_ == 0) {
        // A command frame starts with 0x40 | cmd. Anything else is one of the
        // 0xFF idle bytes a driver sends to give the card clock to wake up.
        if ((mosi & 0xC0) != 0x40) return 0xFF;
        cmd_ = u8(mosi & 0x3F);
        arg_ = 0;
        frameBytes_ = 1;
        return 0xFF;
      }
      if (frameBytes_ <= 4) {
        arg_ = (arg_ << 8) | mosi;
        ++frameBytes_;
        return 0xFF;
      }
      ++frameBytes_;  // the CRC byte, which SPI mode ignores
      execute();
      state_ = State::Responding;
      return nextByte();
    }
    case State::Responding:
      // A multi-block read is a stream of tokens with no gaps, so the only way
      // a driver can interrupt it is the stop command, which arrives here. The
      // command frame is decoded in place; the rest of it is read as usual.
      if (streamRead_ && mosi == u8(0x40 | CmdStopTransmission)) {
        streamRead_ = false;
        out_.clear();
        outPos_ = 0;
        cmd_ = CmdStopTransmission;
        arg_ = 0;
        frameBytes_ = 1;
        state_ = State::Command;
        return 0xFF;
      }
      return nextByte();
    case State::Writing:
      return nextByte();
    case State::Receiving:
      return receiveWrite(mosi);
  }
  return 0xFF;
}

u8 SdCard::nextByte() {
  if (outPos_ < out_.size()) return out_[outPos_++];
  out_.clear();
  outPos_ = 0;
  if (streamRead_) {
    // CMD18 has no length, so the next block is produced only when the driver
    // has clocked the previous one out. Running past the end of the image stops
    // the stream, which is what a card does at the end of its address space.
    if (inRange(streamAddr_)) {
      queueBlock(streamAddr_);
      streamAddr_ += 512;
      return out_[outPos_++];
    }
    streamRead_ = false;
  }
  if (state_ == State::Responding || state_ == State::Writing) {
    state_ = nextState_;
    nextState_ = State::Command;
    frameBytes_ = 0;
  }
  return 0xFF;
}

void SdCard::execute() {
  nextState_ = State::Command;

  if (cmd_ != CmdAppCmd) {
    const bool wasApp = appCmd_;
    appCmd_ = false;
    if (cmd_ == CmdSendOpCondA && wasApp) {
      // ACMD41 is the initialisation handshake: the HCS bit asks for block
      // addressing, which is what makes the card an SDHC.
      sdhc_ = (arg_ & 0x40000000u) != 0;
      idle_ = false;
      queue({R1Ready});
      return;
    }
    if (cmd_ == CmdSendOpCondA) {
      queue({R1Illegal});  // ACMD41 without the CMD55 prefix
      return;
    }
  }

  switch (cmd_) {
    case CmdGoIdle:
      idle_ = true;
      sdhc_ = false;
      queue({R1Idle});
      break;
    case CmdSendIfCond:
      // R7: the voltage accepted, so a driver knows the card speaks 2.7-3.6 V.
      queue({R1Idle, 0x00, 0x00, 0x01, 0xAA});
      break;
    case CmdAppCmd:
      appCmd_ = true;
      queue({R1Idle});
      break;
    case CmdSendOpCond:
      queue({R1Idle});  // the MMC-era form; SD cards keep asking for ACMD41
      break;
    case CmdReadOcr:
      queue({u8(idle_ ? R1Idle : R1Ready), 0x40, u8(sdhc_ ? 0xFF : 0x80), 0x80,
             0x00});
      break;
    case CmdSendCsd:
      queueCsd();
      break;
    case CmdSendCid:
      queueCid();
      break;
    case CmdSetBlockLen:
    case CmdSetBlockCount:
    case CmdCrcOnOff:
      // The block length is fixed at 512 by the SDHC part of the protocol and
      // the block count only announces how many CMD18/CMD25 blocks to expect,
      // so both are accepted and remembered as nothing.
      queue({R1Ready});
      break;
    case CmdSendStatus:
      queue({u8(idle_ ? R1Idle : R1Ready), 0x00});
      break;
    case CmdReadSingle: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      out_.clear();
      outPos_ = 0;
      out_.push_back(R1Ready);
      out_.push_back(0xFF);  // one byte of latency before the token
      queueBlock(addr);
      break;
    }
    case CmdReadMultiple: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      // The first block goes out with the response; every later one is produced
      // in nextByte() so the image is never copied whole.
      streamRead_ = true;
      streamAddr_ = addr + 512;
      out_.clear();
      outPos_ = 0;
      out_.push_back(R1Ready);
      out_.push_back(0xFF);
      queueBlock(addr);
      break;
    }
    case CmdStopTransmission:
      // R1b: the response byte, then a stuff byte, then the card is idle. The
      // busy bytes a real card pulls low are skipped because the transfer here
      // has no duration.
      streamRead_ = false;
      multiWrite_ = false;
      queue({R1Ready, 0xFF});
      break;
    case CmdWriteBlock: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      writeAddr_ = addr;
      multiWrite_ = false;
      queue({R1Ready});
      nextState_ = State::Receiving;
      break;
    }
    case CmdWriteMultiple: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      writeAddr_ = addr;
      multiWrite_ = true;
      queue({R1Ready});
      nextState_ = State::Receiving;
      break;
    }
    default:
      queue({R1Illegal});
      break;
  }
}

void SdCard::dataResponse() {
  // The data-response token: accepted, then a couple of busy bytes and the card
  // is ready again. The write stays in the in-memory image.
  queue({DataAccepted, 0xFF, 0x00});
  state_ = State::Writing;
}

u8 SdCard::receiveWrite(u8 mosi) {
  if (!tokenSeen_) {
    if (multiWrite_ && mosi == StopToken) {
      // CMD25 ends with the stop token rather than a length; the driver still
      // owes a CMD12, which is answered from the command state.
      multiWrite_ = false;
      nextState_ = State::Command;
      dataResponse();
      return nextByte();
    }
    if (mosi != DataToken) return 0xFF;  // still waiting for the data token
    tokenSeen_ = true;
    writeBuf_.clear();
    return 0xFF;
  }
  writeBuf_.push_back(mosi);
  if (writeBuf_.size() < 512u + 2u) return 0xFF;  // payload plus two CRC bytes

  if (inRange(writeAddr_)) {
    std::copy(writeBuf_.begin(), writeBuf_.begin() + 512, image_.begin() + writeAddr_);
  }
  writeAddr_ += 512;
  tokenSeen_ = false;
  writeBuf_.clear();
  nextState_ = multiWrite_ ? State::Receiving : State::Command;
  dataResponse();
  return nextByte();
}

void SdCard::queueCsd() {
  u8 csd[16];
  std::copy(std::begin(kCsdTemplate), std::end(kCsdTemplate), csd);
  // C_SIZE counts 512 KB units, so it is sectors / 1024 - 1.
  const u32 csize = sectors() >= 1024 ? sectors() / 1024 - 1 : 0;
  csd[7] = u8((csize >> 16) & 0x3F);
  csd[8] = u8((csize >> 8) & 0xFF);
  csd[9] = u8(csize & 0xFF);

  std::vector<u8> block;
  block.reserve(3 + 16 + 2);
  block.push_back(R1Ready);
  block.push_back(0xFF);
  block.push_back(DataToken);
  block.insert(block.end(), std::begin(csd), std::end(csd));
  block.push_back(0xFF);
  block.push_back(0xFF);
  queue(block);
}

void SdCard::queueCid() {
  std::vector<u8> block;
  block.reserve(3 + 16 + 2);
  block.push_back(R1Ready);
  block.push_back(0xFF);
  block.push_back(DataToken);
  block.insert(block.end(), std::begin(kCid), std::end(kCid));
  block.push_back(0xFF);
  block.push_back(0xFF);
  queue(block);
}

} // namespace rainbow
