#include "core/upd765.h"

#include <algorithm>

namespace rainbow {
namespace {

// Opcodes are compared on their low five bits so the two variants of a command
// (normal and "MT" for multi-track) share one implementation.
constexpr u8 OpReadTrack = 0x02;
constexpr u8 OpSpecify = 0x03;
constexpr u8 OpSenseDrive = 0x04;
constexpr u8 OpWriteData = 0x05;
constexpr u8 OpReadData = 0x06;
constexpr u8 OpRecalibrate = 0x07;
constexpr u8 OpSenseInterrupt = 0x08;
constexpr u8 OpWriteDeleted = 0x09;
constexpr u8 OpReadId = 0x0A;
constexpr u8 OpReadDeleted = 0x0C;
constexpr u8 OpFormatTrack = 0x0D;
constexpr u8 OpSeek = 0x0F;

size_t commandLength(u8 opcode) {
  switch (opcode & 0x1F) {
    case OpSpecify: return 3;
    case OpSenseDrive: return 2;
    case OpWriteData:
    case OpWriteDeleted:
    case OpReadData:
    case OpReadDeleted:
    case OpReadTrack:
    case 0x11: // SCAN EQUAL
    case 0x19: // SCAN LOW OR EQUAL
    case 0x1D: // SCAN HIGH OR EQUAL
      return 9;
    case OpRecalibrate: return 2;
    case OpSenseInterrupt: return 1;
    case OpReadId: return 2;
    case OpFormatTrack: return 6;
    case OpSeek: return 3;
    default: return 1;
  }
}

constexpr u8 St0Interrupt = 0x20; // IC = normal termination, seek end
constexpr u8 St0Abnormal = 0x40;  // IC = abnormal termination
constexpr u8 St0Invalid = 0x80;   // IC = invalid command

} // namespace

void Upd765::reset() {
  phase_ = Phase::Idle;
  command_.clear();
  buffer_.clear();
  result_.clear();
  cursor_ = 0;
  cpuToFdc_ = false;
  cylinder_ = 0;
  head_ = 0;
  unit_ = 0;
  seekPending_ = false;
  seekStatus_ = 0;
  seekCylinder_ = 0;
  status0_ = status1_ = status2_ = 0;
  lastC_ = lastH_ = 0;
  lastR_ = 1;
  lastN_ = 2;
  motor_ = false;
}

void Upd765::setDisk(formats::DiskImage* disk) {
  disk_ = disk;
  if (disk_ == nullptr) {
    // Nothing to seek to, but keep the head where the ROM last left it so a
    // freshly inserted disk starts from a clean state.
    cylinder_ = 0;
    head_ = 0;
  }
}

u8 Upd765::readStatus() const {
  u8 msr = 0;
  if (hasDisk()) msr |= 0x01; // drive 0 busy
  if (hasDisk() && disk_->sides > 1) msr |= 0x02;

  switch (phase_) {
    case Phase::Idle:
    case Phase::Command:
      msr |= MsrRqm;
      break;
    case Phase::Execution:
      msr |= MsrBusy | MsrNdma | MsrRqm;
      if (!cpuToFdc_) msr |= MsrDio;
      break;
    case Phase::Result:
      msr |= MsrBusy | MsrRqm | MsrDio;
      break;
  }
  return msr;
}

u8 Upd765::readData() {
  if (phase_ == Phase::Execution && !cpuToFdc_) {
    const u8 value = cursor_ < buffer_.size() ? buffer_[cursor_] : 0xFF;
    ++cursor_;
    if (cursor_ >= buffer_.size()) endExecution();
    return value;
  }
  if (phase_ == Phase::Result && cursor_ < result_.size()) {
    const u8 value = result_[cursor_++];
    if (cursor_ >= result_.size()) {
      phase_ = Phase::Idle;
      command_.clear();
      result_.clear();
      cursor_ = 0;
    }
    return value;
  }
  return 0xFF;
}

void Upd765::writeCommand(u8 value) {
  if (phase_ == Phase::Result || phase_ == Phase::Execution) return;
  if (phase_ == Phase::Idle) {
    command_.clear();
    phase_ = Phase::Command;
  }
  command_.push_back(value);
  if (command_.size() >= commandLength(command_[0])) finishCommand();
}

void Upd765::writeData(u8 value) {
  if (phase_ != Phase::Execution || !cpuToFdc_) return;
  buffer_.push_back(value);
  const u8 opcode = command_[0] & 0x1F;
  size_t expected = 0;
  if (opcode == OpFormatTrack) {
    expected = size_t(command_[3]) * 4;
  } else {
    const u8 first = command_[4];
    const u8 sizeCode = command_[5];
    const u8 last = command_[6];
    const size_t count = last >= first ? size_t(last) - first + 1 : 0;
    expected = count * sectorLength(sizeCode);
  }
  if (expected != 0 && buffer_.size() >= expected) endExecution();
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void Upd765::finishCommand() {
  const u8 opcode = command_[0] & 0x1F;
  unit_ = command_[0] & 0x03;

  switch (opcode) {
    case OpSpecify:
      // Timing parameters only; the emulator runs at full speed anyway.
      phase_ = Phase::Idle;
      command_.clear();
      return;

    case OpSenseDrive:
      unit_ = command_[1] & 0x03;
      head_ = (command_[1] >> 2) & 0x01;
      status0_ = u8(St0Interrupt | unit_);
      pushResult(status0_, driveStatus(), 0, 0, 0, 0, 0);
      return;

    case OpSenseInterrupt:
      if (!seekPending_) {
        pushResult(St0Invalid, 0, 0, 0, 0, 0, 0);
        return;
      }
      seekPending_ = false;
      pushResult(u8(St0Interrupt | (seekStatus_ & 0x03)), 0, 0, u8(seekCylinder_), 0, 0, 0);
      return;

    case OpRecalibrate:
      head_ = (command_[1] >> 2) & 0x01;
      seekTo(0);
      return;

    case OpSeek:
      head_ = (command_[1] >> 2) & 0x01;
      seekTo(command_[2]);
      return;

    case OpReadId:
      head_ = (command_[1] >> 2) & 0x01;
      readId();
      return;

    case OpReadData:
    case OpReadDeleted:
    case OpReadTrack:
      head_ = (command_[1] >> 2) & 0x01;
      if (opcode == OpReadTrack) {
        readTrack();
      } else {
        readSectors(command_[2], command_[3], command_[4], command_[5], command_[6]);
      }
      return;

    case OpWriteData:
    case OpWriteDeleted:
      head_ = (command_[1] >> 2) & 0x01;
      if (!hasDisk()) {
        failNoDisk(command_[2], command_[3], command_[4], command_[5]);
        return;
      }
      startExecution();
      return;

    case OpFormatTrack:
      head_ = (command_[1] >> 2) & 0x01;
      if (!hasDisk()) {
        failNoDisk(u8(cylinder_), u8(head_), 0, command_[2]);
        return;
      }
      startExecution();
      return;

    default:
      status0_ = u8(St0Invalid | unit_);
      pushResult(status0_, 0x80, 0, 0, 0, 0, 0);
      return;
  }
}

// No image inserted: the controller reports "no data" instead of pretending the
// transfer happened, which is what the +3 ROM turns into "Drive not ready".
void Upd765::failNoDisk(u8 c, u8 h, u8 r, u8 n) {
  pushResult(u8(St0Abnormal | (head_ ? 0x04 : 0x00) | unit_), 0x04, 0x10, c, h, r, n);
}

void Upd765::seekTo(u8 target) {
  cylinder_ = target;
  seekCylinder_ = target;
  seekStatus_ = u8(unit_);
  seekPending_ = true;
  phase_ = Phase::Idle;
  command_.clear();
}

void Upd765::readId() {
  formats::DskTrack* track = currentTrack();
  if (track == nullptr || track->sectors.empty()) {
    status0_ = u8(St0Abnormal | unit_);
    pushResult(status0_, 0x04, 0, u8(cylinder_), u8(head_), 0, 0);
    return;
  }
  const formats::DskSector& sector = track->sectors.front();
  lastC_ = sector.c;
  lastH_ = sector.h;
  lastR_ = sector.r;
  lastN_ = sector.n;
  status0_ = u8(St0Interrupt | (head_ ? 0x04 : 0x00) | unit_);
  pushResult(status0_, 0, 0, sector.c, sector.h, sector.r, sector.n);
}

void Upd765::readSectors(u8 cylinder, u8 head, u8 first, u8 sizeCode, u8 last) {
  const formats::DskTrack* track = disk_ != nullptr ? disk_->findTrack(cylinder, head) : nullptr;
  if (track == nullptr || track->sectors.empty()) {
    status0_ = u8(St0Abnormal | (head_ ? 0x04 : 0x00) | unit_);
    pushResult(status0_, 0x04, 0, cylinder, head, first, sizeCode);
    return;
  }

  buffer_.clear();
  cursor_ = 0;
  status1_ = 0;
  status2_ = 0;
  lastC_ = cylinder;
  lastH_ = head;
  lastR_ = first;
  lastN_ = sizeCode;

  for (u16 id = first; id <= last; ++id) {
    const formats::DskSector* sector = track->findSector(cylinder, head, u8(id), sizeCode);
    if (sector == nullptr) {
      status1_ |= 0x04; // no data: the sector the loader wanted is missing
      break;
    }
    lastC_ = sector->c;
    lastH_ = sector->h;
    lastR_ = sector->r;
    lastN_ = sector->n;
    if (sector->deleted) status2_ |= 0x40; // control mark
    buffer_.insert(buffer_.end(), sector->data.begin(), sector->data.end());
  }

  cpuToFdc_ = false;
  phase_ = Phase::Execution;
  if (buffer_.empty()) {
    endExecution();
    return;
  }
}

void Upd765::readTrack() {
  const formats::DskTrack* track = currentTrack();
  if (track == nullptr || track->sectors.empty()) {
    status0_ = u8(St0Abnormal | (head_ ? 0x04 : 0x00) | unit_);
    pushResult(status0_, 0x04, 0, u8(cylinder_), u8(head_), 0, 0);
    return;
  }
  buffer_.clear();
  cursor_ = 0;
  status1_ = 0;
  status2_ = 0;
  for (const formats::DskSector& sector : track->sectors) {
    buffer_.insert(buffer_.end(), sector.data.begin(), sector.data.end());
  }
  lastC_ = track->sectors.front().c;
  lastH_ = track->sectors.front().h;
  lastR_ = track->sectors.front().r;
  lastN_ = track->sectors.front().n;
  cpuToFdc_ = false;
  phase_ = Phase::Execution;
}

void Upd765::startExecution() {
  buffer_.clear();
  cursor_ = 0;
  status1_ = 0;
  status2_ = 0;
  cpuToFdc_ = true;
  phase_ = Phase::Execution;

  const u8 opcode = command_[0] & 0x1F;
  if (opcode == OpFormatTrack) {
    // Nothing to prepare: the CPU supplies the sector ids in the data phase.
    return;
  }
  lastC_ = command_[2];
  lastH_ = command_[3];
  lastR_ = command_[4];
  lastN_ = command_[5];
}

void Upd765::endExecution() {
  const u8 opcode = command_[0] & 0x1F;

  if (opcode == OpFormatTrack) {
    formatTrack();
    return;
  }

  if (cpuToFdc_) {
    writeSectors();
    return;
  }

  // READ DATA / READ TRACK finished: report where it stopped.
  const u8 status = u8(status1_ == 0 ? St0Interrupt : St0Abnormal);
  pushResult(u8(status | (head_ ? 0x04 : 0x00) | unit_), status1_, status2_, lastC_, lastH_,
             lastR_, lastN_);
}

void Upd765::writeSectors() {
  formats::DskTrack& track = trackFor(lastC_, lastH_);
  const u8 first = command_[4];
  const u8 sizeCode = command_[5];
  const u8 last = command_[6];
  const size_t length = sectorLength(sizeCode);

  size_t offset = 0;
  for (u16 id = first; id <= last && offset < buffer_.size(); ++id) {
    const size_t take = std::min(length, buffer_.size() - offset);
    formats::DskSector* sector = track.findSector(lastC_, lastH_, u8(id), sizeCode);
    if (sector == nullptr) {
      formats::DskSector created;
      created.c = lastC_;
      created.h = lastH_;
      created.r = u8(id);
      created.n = sizeCode;
      created.data.assign(length, 0xE5);
      track.sectors.push_back(created);
      sector = &track.sectors.back();
    }
    sector->data.assign(buffer_.begin() + std::ptrdiff_t(offset),
                        buffer_.begin() + std::ptrdiff_t(offset + take));
    sector->data.resize(length, 0xE5);
    offset += take;
  }
  track.formatted = true;

  pushResult(u8(St0Interrupt | (head_ ? 0x04 : 0x00) | unit_), 0, 0, lastC_, lastH_, lastR_,
             lastN_);
}

void Upd765::formatTrack() {
  const u8 sizeCode = command_[2];
  const u8 count = command_[3];
  const u8 filler = command_[5];

  formats::DskTrack& track = trackFor(cylinder_, head_);
  track.sectors.clear();
  const size_t length = sectorLength(sizeCode);
  for (u8 index = 0; index < count; ++index) {
    const size_t entry = size_t(index) * 4;
    formats::DskSector sector;
    if (entry + 3 < buffer_.size()) {
      sector.c = buffer_[entry];
      sector.h = buffer_[entry + 1];
      sector.r = buffer_[entry + 2];
      sector.n = buffer_[entry + 3];
    } else {
      sector.c = u8(cylinder_);
      sector.h = u8(head_);
      sector.r = u8(index + 1);
      sector.n = sizeCode;
    }
    sector.data.assign(length, filler);
    track.sectors.push_back(std::move(sector));
  }
  track.formatted = !track.sectors.empty();

  pushResult(u8(St0Interrupt | (head_ ? 0x04 : 0x00) | unit_), 0, 0, u8(cylinder_), u8(head_),
             u8(count), sizeCode);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Upd765::pushResult(u8 status0, u8 status1, u8 status2, u8 c, u8 h, u8 r, u8 n) {
  status0_ = status0;
  status1_ = status1;
  status2_ = status2;
  result_ = {status0, status1, status2, c, h, r, n};
  cursor_ = 0;
  phase_ = Phase::Result;
}

u8 Upd765::driveStatus() const {
  u8 st3 = u8(unit_ & 0x03);
  if (head_ != 0) st3 |= 0x04;
  if (hasDisk()) {
    if (disk_->sides > 1) st3 |= 0x08; // two-sided
    st3 |= 0x20;                       // ready
  }
  if (cylinder_ == 0) st3 |= 0x10; // track 0
  return st3;
}

formats::DskTrack* Upd765::currentTrack() {
  if (disk_ == nullptr) return nullptr;
  return disk_->findTrack(cylinder_, head_);
}

formats::DskTrack& Upd765::trackFor(int cylinder, int head) {
  return disk_->ensureTrack(cylinder, head);
}

} // namespace rainbow
