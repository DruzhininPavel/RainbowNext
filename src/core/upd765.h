#pragma once

#include <string>
#include <vector>

#include "core/types.h"
#include "formats/dsk.h"

namespace rainbow {

// NEC µPD765 as wired into the +2A/+3: two ports, non-DMA mode only, one or
// two single-sided drives. The controller is a small state machine because the
// +3's ROM drives it byte by byte, polling the main status register between
// every transfer.
class Upd765 {
 public:
  // Main status register bits.
  static constexpr u8 MsrRqm = 0x80;  // data register ready
  static constexpr u8 MsrDio = 0x40;  // 1 = controller -> CPU
  static constexpr u8 MsrNdma = 0x20; // execution phase in non-DMA mode
  static constexpr u8 MsrBusy = 0x10; // command in progress

  void reset();
  // The controller does not own the image; it must outlive the controller.
  void setDisk(formats::DiskImage* disk);
  const formats::DiskImage* disk() const { return disk_; }
  bool hasDisk() const { return disk_ != nullptr && !disk_->empty(); }
  void eject() { disk_ = nullptr; }

  void setMotor(bool on) { motor_ = on; }
  bool motor() const { return motor_; }

  u8 readStatus() const;
  u8 readData();
  void writeCommand(u8 value);
  void writeData(u8 value);

  // Exposed for the debugger overlay and the tests.
  int cylinder() const { return cylinder_; }
  int head() const { return head_; }
  u8 status0() const { return status0_; }
  u8 status1() const { return status1_; }
  u8 status2() const { return status2_; }
  bool busy() const { return phase_ != Phase::Idle; }

  // The disk image itself is not a save state, only where the controller is in
  // a command; the image is reattached with loadDisk.
  template <class Ar> void serialize(Ar& ar) {
    ar(motor_);
    ar(phase_);
    ar(command_);
    ar(buffer_);
    ar(cursor_);
    ar(result_);
    ar(cpuToFdc_);
    ar(cylinder_);
    ar(head_);
    ar(unit_);
    ar(seekPending_);
    ar(seekStatus_);
    ar(seekCylinder_);
    ar(status0_);
    ar(status1_);
    ar(status2_);
    ar(lastC_);
    ar(lastH_);
    ar(lastR_);
    ar(lastN_);
  }

 private:
  enum class Phase { Idle, Command, Execution, Result };

  void finishCommand();
  void startExecution();
  void endExecution();
  // `count` is how many bytes the real chip hands back for this command: seven
  // for the data/ID/format commands, two for SENSE INTERRUPT STATUS, one for
  // SENSE DRIVE STATUS and for an invalid command. Getting this wrong leaves
  // the +3 ROM reading padding as status and retrying forever.
  void pushResult(u8 status0, u8 status1, u8 status2, u8 c, u8 h, u8 r, u8 n,
                  size_t count = 7);
  void failNoDisk(u8 c, u8 h, u8 r, u8 n);
  void readSectors(u8 cylinder, u8 head, u8 first, u8 sizeCode, u8 last);
  void writeSectors();
  void formatTrack();
  void readTrack();
  void readId();
  void seekTo(u8 cylinder);
  u8 driveStatus() const;
  formats::DskTrack* currentTrack();
  formats::DskTrack& trackFor(int cylinder, int head);
  size_t sectorLength(u8 sizeCode) const { return size_t(128) << (sizeCode & 0x07); }

  formats::DiskImage* disk_ = nullptr;
  bool motor_ = false;

  Phase phase_ = Phase::Idle;
  std::vector<u8> command_; // opcode plus parameters
  std::vector<u8> buffer_;  // data phase bytes
  size_t cursor_ = 0;
  std::vector<u8> result_;
  bool cpuToFdc_ = false;

  int cylinder_ = 0;
  int head_ = 0;
  int unit_ = 0;
  bool seekPending_ = false;
  u8 seekStatus_ = 0;
  int seekCylinder_ = 0;

  u8 status0_ = 0;
  u8 status1_ = 0;
  u8 status2_ = 0;
  // Track the last transferred sector so a short read still reports something
  // sensible, and remember the sector id a write should land in.
  u8 lastC_ = 0;
  u8 lastH_ = 0;
  u8 lastR_ = 1;
  u8 lastN_ = 2;
};

} // namespace rainbow
