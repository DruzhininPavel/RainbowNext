#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow {

// An SD card as an SPI device. The card is a plain sector image in memory, so
// it is deterministic and testable without a file: feed the state machine the
// bytes a driver would send and check the answers.
//
// The protocol is the one a DivMMC or the Next's boot ROM speaks: idle bytes
// until a 6-byte command frame (0x40 | cmd, four argument bytes, a CRC that is
// ignored in SPI mode), then an R1 response, then whatever the command returns.
// Commands are answered in the order they arrive, with no timing, because the
// emulated SPI clock is not modelled -- a real card needs a few hundred clock
// cycles to program, and a driver that polls for that simply sees the answer at
// once.
//
// The card is deliberately not a peripheral: it has no ports and no bus
// presence of its own, so the Next's built-in interface, a future module that
// brings its own ports, and the tests can all share one implementation.
class SdCard {
public:
  // 512-byte sectors. Anything that is not a multiple of the sector size is
  // rejected rather than padded, because a truncated image would look corrupt
  // to the driver in a way that is hard to diagnose.
  bool load(std::vector<u8> image, std::string& error);
  void eject();
  bool inserted() const { return !image_.empty(); }
  u32 sectors() const { return u32(image_.size() / 512); }
  const std::vector<u8>& image() const { return image_; }

  // Card select is active low. While it is deasserted the card ignores the bus
  // and leaves MISO high, which is how a driver finds out it is alone.
  void select(bool asserted);
  bool selected() const { return selected_; }

  // Shift one byte out while shifting one in; returns MISO.
  u8 transfer(u8 mosi);

  // Back to power-on state. The image survives, the protocol state does not.
  void reset();

  // True while a multi-block read is streaming. A driver that stops early has
  // to send CMD12, so the frontend can show a stuck transfer.
  bool streaming() const { return streamRead_; }

private:
  enum class State { Command, Responding, Receiving, Writing };

  void execute();
  void queue(const std::vector<u8>& bytes);
  void queueBlock(u32 addr);
  void queueCsd();
  void queueCid();
  u8 nextByte();
  u8 receiveWrite(u8 mosi);
  void dataResponse();
  u32 address() const { return sdhc_ ? arg_ * 512u : arg_; }
  bool inRange(u32 addr) const { return u64(addr) + 512 <= image_.size(); }

  std::vector<u8> image_;
  bool selected_ = false;

  State state_ = State::Command;
  // Where the state machine goes once the bytes queued for this command have
  // been shifted out.
  State nextState_ = State::Command;
  u8 cmd_ = 0;
  u32 arg_ = 0;
  int frameBytes_ = 0;
  bool appCmd_ = false;
  bool idle_ = true;
  bool sdhc_ = false;

  std::vector<u8> out_;
  size_t outPos_ = 0;

  // CMD18: blocks are produced as the driver clocks them out, so a card image
  // is never copied into a buffer whole.
  bool streamRead_ = false;
  u32 streamAddr_ = 0;

  // CMD25: the token/payload loop repeats until the stop token or CMD12.
  bool multiWrite_ = false;

  // A write in progress: the token has been seen, the payload is arriving.
  bool tokenSeen_ = false;
  u32 writeAddr_ = 0;
  std::vector<u8> writeBuf_;
};

} // namespace rainbow
