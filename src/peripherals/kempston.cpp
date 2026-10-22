#include "peripherals/kempston.h"

namespace mynext {

KempstonJoystick::KempstonJoystick() {
  info_.id = "kempston";
  info_.name = "Kempston Joystick";
  info_.version = "1.0";
  info_.author = "myNext";
  info_.description =
      "The standard joystick interface. Reads port 0x1F; bits 5-7 stay clear, "
      "which is how software detects that it is plugged in.";
  info_.ports = {0x1F};
}

bool KempstonJoystick::ioRead(u16 port, u32, u8& value) {
  if ((port & 0x00E0) != 0x0000) return false;
  value = u8(host_ ? host_->joystickState() : 0x00) & 0x1F;
  return true;
}

} // namespace mynext
