#pragma once

#include <string>
#include <vector>

#include "core/z80.h"

namespace mynext::formats {

// A machine-independent snapshot: CPU registers plus a full 128K RAM image
// (8 x 16K banks). 48K snapshots leave banks 1,3,4,6,7 zeroed and place the
// 48K image in banks 5,2,0, which is exactly how a 48K Spectrum maps.
struct Snapshot {
  Z80::Registers regs{};
  std::vector<u8> ram;
  u8 border = 0;
  u8 port7ffd = 0;
  bool is128 = false;
};

} // namespace mynext::formats
