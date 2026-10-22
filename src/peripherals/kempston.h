#pragma once

#include "core/peripheral.h"

namespace rainbow {

// Kempston joystick: one read port that decodes A7-A5 as zero, so 0x1F and its
// mirrors answer. Bits 5-7 stay clear, which is how a program detects that the
// interface is plugged in.
class KempstonJoystick : public Peripheral {
public:
  KempstonJoystick();

  const PeripheralInfo& info() const override { return info_; }
  void attachHost(const HostServices& host) override { host_ = &host; }
  bool ioRead(u16 port, u32 t, u8& value) override;

private:
  PeripheralInfo info_;
  const HostServices* host_ = nullptr;
};

} // namespace rainbow
