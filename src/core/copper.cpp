#include "core/copper.h"

namespace rainbow {

void Copper::reset() {
  program_.fill(0);
  index_ = 0;
  mode_ = 0;
  cpc_ = 0;
  pendingMsb_ = 0;
  waitLine_ = 0;
  waitHpos_ = 0;
}

void Copper::writeData8(u8 value) {
  program_[size_t(index_)] = value;
  index_ = u16((index_ + 1) & (ProgramSize - 1));
}

void Copper::writeData16(u8 value) {
  if ((index_ & 1) == 0) {
    pendingMsb_ = value;
  } else {
    program_[size_t(index_ - 1)] = pendingMsb_;
    program_[size_t(index_)] = value;
  }
  index_ = u16((index_ + 1) & (ProgramSize - 1));
}

void Copper::writeControl(u8 value) {
  index_ = u16(((u16(value & 0x07) << 8) | (index_ & 0xFF)) & 0x7FF);
  const u8 mode = u8((value >> 6) & 0x03);
  // Repeating the mode only moves the write index; the running program keeps
  // going so a program can be uploaded without restarting it.
  if (mode == mode_) return;
  mode_ = mode;
  if (mode == 1 || mode == 3) cpc_ = 0;
}

} // namespace rainbow
