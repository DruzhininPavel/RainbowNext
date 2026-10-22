#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/types.h"

namespace mynext::formats {

// A .nex container: a ZX Spectrum Next program together with the RAM banks it
// needs, an optional loading screen and the NextReg state its author asked for.
// The layout follows the V1.2/V1.3 specification, which is what the official
// nexload reads; see docs/REFERENCES.md.
struct NexBank {
  int number = 0;         // 0-111, the 16K bank this data belongs to
  std::vector<u8> data;   // 16384 bytes
};

// A .nex can carry every 16K bank of a 1.75M machine.
constexpr int NexMaxBanks = 112;

struct NexFile {
  int major = 1;
  int minor = 2;
  u8 ramRequired = 0;         // 0 = 768K, 1 = 1792K
  u8 bankCount = 0;           // as declared; the bank flags below are the truth
  u8 screenFlags = 0;         // bit 7 no palette, 4 LoRes, 2 ULA, 1 Layer 2, ...
  u8 border = 0;
  u16 sp = 0;
  u16 pc = 0;                 // 0 means "load but do not run"
  u8 entryBank = 0;           // mapped at 0xC000 before the jump
  u16 fileHandleAddress = 0;
  bool preserveNextRegs = false;
  u8 coreMajor = 0, coreMinor = 0, coreSubminor = 0;
  u8 timexColour = 0;         // also the Layer 2 palette offset for 320x256
  u8 expBusDisable = 0;       // V1.3: 0 asks the loader to disable the bus
  u8 loadingBar = 0, loadingBarColour = 0, loadingDelay = 0, startDelay = 0;

  // One flag per bank, in plain numeric order (bank 5 is at index 5).
  std::array<bool, NexMaxBanks> bankPresent{};

  bool hasPalette = false;
  std::array<u16, 256> palette{};   // RGB333, one entry per index
  std::vector<u8> layer2Screen;     // 49152 bytes, belongs in banks 9, 10, 11
  std::vector<u8> ulaScreen;        // 6912 bytes, belongs in bank 5 at 0x4000
  std::vector<u8> loresScreen;      // 12288 bytes
  std::vector<u8> hiresScreen;      // 12288 bytes
  std::vector<u8> hicolScreen;      // 12288 bytes

  // In file order -- 5, 2, 0, then 3 through 111 -- so a later entry overwrites
  // an earlier one, exactly as the loader reads them.
  std::vector<NexBank> banks;
};

// The header is 512 bytes and carries a "Next" signature. Everything after it
// is optional and its layout is driven by the header, so a truncated file is
// rejected rather than half-loaded.
bool loadNex(const std::vector<u8>& data, NexFile& out, std::string& error);

} // namespace mynext::formats
