#include "formats/nex.h"

#include <cstring>

namespace mynext::formats {
namespace {

constexpr size_t HeaderSize = 512;
constexpr size_t BankSize = 16384;
constexpr int MaxBanks = NexMaxBanks;
constexpr size_t PaletteSize = 512;

u16 word(const std::vector<u8>& d, size_t offset) {
  return u16(d[offset] | (u16(d[offset + 1]) << 8));
}

// Takes `count` bytes off the front of the payload, so the loader never reads
// past the end of a file whose header promised more than it carries.
bool take(const std::vector<u8>& data, size_t& cursor, size_t count, std::vector<u8>& out,
          const char* what, std::string& error) {
  if (cursor + count > data.size()) {
    error = std::string("NEX file is truncated: it ends inside the ") + what;
    return false;
  }
  out.assign(data.begin() + std::ptrdiff_t(cursor),
             data.begin() + std::ptrdiff_t(cursor + count));
  cursor += count;
  return true;
}

} // namespace

bool loadNex(const std::vector<u8>& data, NexFile& out, std::string& error) {
  out = NexFile{};
  if (data.size() < HeaderSize) {
    error = "NEX file is smaller than its 512-byte header";
    return false;
  }
  if (std::memcmp(data.data(), "Next", 4) != 0) {
    error = "not a NEX file: the \"Next\" signature is missing";
    return false;
  }

  // The version string is "V1.2": the digits sit at offsets 5 and 7.
  const bool digits = data[5] >= '0' && data[5] <= '9' && data[7] >= '0' && data[7] <= '9';
  out.major = digits ? int(data[5] - '0') : 1;
  out.minor = digits ? int(data[7] - '0') : 0;
  if (out.major != 1) {
    error = "unsupported NEX version " + std::to_string(out.major) + "." +
            std::to_string(out.minor) + "; this loader understands 1.x";
    return false;
  }

  out.ramRequired = data[8];
  out.bankCount = data[9];
  out.screenFlags = data[10];
  out.border = u8(data[11] & 0x07);
  out.sp = word(data, 12);
  out.pc = word(data, 14);
  for (int bank = 0; bank < MaxBanks; ++bank) {
    out.bankPresent[size_t(bank)] = data[size_t(18 + bank)] != 0;
  }
  out.loadingBar = data[130];
  out.loadingBarColour = data[131];
  out.loadingDelay = data[132];
  out.startDelay = data[133];
  out.preserveNextRegs = data[134] != 0;
  out.coreMajor = u8(data[135] & 0x0F);
  out.coreMinor = u8(data[136] & 0x0F);
  out.coreSubminor = data[137];
  out.timexColour = data[138];
  out.entryBank = data[139];
  out.fileHandleAddress = word(data, 140);
  out.expBusDisable = data[142];

  if (out.entryBank >= MaxBanks) {
    error = "NEX entry bank " + std::to_string(out.entryBank) + " is out of range";
    return false;
  }

  size_t cursor = HeaderSize;

  // No loading screen means no palette block either: the loader skips straight
  // to the banks. The palette is also absent for the ULA, Timex HiRes and
  // HiCol screens, which use the fixed ULA palette -- bit 7 says so explicitly
  // and bits 1, 3 and 4 imply it.
  if (out.screenFlags != 0) {
    const bool paletteBlock = (out.screenFlags & 0x80) == 0 && (out.screenFlags & 0x1A) == 0;
    if (paletteBlock) {
      if (cursor + PaletteSize > data.size()) {
        error = "NEX file is truncated: it ends inside the palette";
        return false;
      }
      out.hasPalette = true;
      for (int entry = 0; entry < 256; ++entry) {
        const u8 low = data[cursor + size_t(entry) * 2];
        const u8 high = data[cursor + size_t(entry) * 2 + 1];
        // The two bytes are RRRGGGBB and the ninth bit, which the hardware
        // takes as the low bit of the three-bit blue channel.
        const u16 blue = u16(((low & 0x03) << 1) | (high & 0x01));
        out.palette[size_t(entry)] =
            u16(((low & 0xE0u) << 1) | (((low & 0x1Cu) >> 2) << 3) | blue);
      }
      cursor += PaletteSize;
    }

    if ((out.screenFlags & 0x01) != 0 &&
        !take(data, cursor, 49152, out.layer2Screen, "Layer 2 loading screen", error)) {
      return false;
    }
    if ((out.screenFlags & 0x02) != 0 &&
        !take(data, cursor, 6912, out.ulaScreen, "ULA loading screen", error)) {
      return false;
    }
    if ((out.screenFlags & 0x04) != 0 &&
        !take(data, cursor, 12288, out.loresScreen, "LoRes loading screen", error)) {
      return false;
    }
    if ((out.screenFlags & 0x08) != 0 &&
        !take(data, cursor, 12288, out.hiresScreen, "Timex HiRes loading screen", error)) {
      return false;
    }
    if ((out.screenFlags & 0x10) != 0 &&
        !take(data, cursor, 12288, out.hicolScreen, "Timex HiCol loading screen", error)) {
      return false;
    }
  }

  // The three banks the loader can show a screen from come first, then the rest
  // in ascending order. The declared bank count is not consulted: nexload walks
  // the flag array too, so a file whose count disagrees still loads.
  auto readBank = [&](int number) {
    NexBank bank;
    bank.number = number;
    if (!take(data, cursor, BankSize, bank.data, "RAM bank data", error)) return false;
    out.banks.push_back(std::move(bank));
    return true;
  };

  for (const int number : {5, 2, 0}) {
    if (out.bankPresent[size_t(number)] && !readBank(number)) return false;
  }
  // The remaining banks follow in ascending order; 5, 2 and 0 have already been
  // read, so they are skipped rather than read a second time.
  for (int number = 1; number < MaxBanks; ++number) {
    if (number == 2 || number == 5) continue;
    if (out.bankPresent[size_t(number)] && !readBank(number)) return false;
  }
  return true;
}

} // namespace mynext::formats
