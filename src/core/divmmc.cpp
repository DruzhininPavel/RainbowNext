#include "core/divmmc.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace rainbow {

namespace {

bool readWholeFile(const std::string& path, std::vector<u8>& data, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (!in.eof() && in.fail()) {
    error = "cannot read " + path;
    return false;
  }
  return true;
}

} // namespace

void DivMmc::hardReset() {
  control_ = 0;
  held_ = false;
  mapPending_ = false;
  unmapPending_ = false;
  std::fill(ram_.begin(), ram_.end(), u8(0));
}

void DivMmc::softReset() {
  control_ = 0;
  held_ = false;
  mapPending_ = false;
  unmapPending_ = false;
}

u32 DivMmc::romPage() const {
  if (romPages() <= 1) return 0;
  return u32((control_ >> 4) & 0x03) % romPages();
}

bool DivMmc::hookEnabled(u16 addr) const {
  u8 points = entryPoints0_;
  u8 bit = 0;
  switch (addr) {
    case 0x0000: bit = 0x01; break;
    case 0x0008: bit = 0x02; break;
    case 0x0010: bit = 0x04; break;
    case 0x0018: bit = 0x08; break;
    case 0x0020: bit = 0x10; break;
    case 0x0028: bit = 0x20; break;
    case 0x0030: bit = 0x40; break;
    case 0x0038: bit = 0x80; break;
    // 0x0066 is the divMMC NMI button's entry point and only fires while that
    // button is held; the machine has no button, so it never does.
    case 0x0066: return false;
    case 0x04C6: points = entryPoints1_; bit = 0x04; break;
    case 0x04D7: points = entryPoints1_; bit = 0x10; break;
    case 0x0562: points = entryPoints1_; bit = 0x08; break;
    case 0x056A: points = entryPoints1_; bit = 0x20; break;
    default:
      // The whole 0x3D00 page is a single entry point, the esxDOS RST 8 hook.
      if ((addr & 0xFF00) == 0x3D00) {
        points = entryPoints1_;
        bit = 0x80;
        break;
      }
      return false;
  }
  if ((points & bit) == 0) return false;
  // 0xB9 only covers the eight RST and reset vectors. A clear bit there means
  // the point fires only while the divMMC's ROM3 is paged -- the 48K ROM on the
  // Next, the +3 DOS ROM elsewhere. That is why RST 0 is hooked on a stock Next
  // and RST 8 is not.
  if (addr < 0x0066) {
    if ((entryPointsValid0_ & bit) == 0 && !rom3Present_) return false;
  } else if (!rom3Present_) {
    return false;
  }
  return true;
}

bool DivMmc::hookInstant(u16 addr) const {
  if ((addr & 0xFF00) == 0x3D00) return true;
  if (addr >= 0x0066) return false;
  const u8 bit = u8(1u << ((addr >> 3) & 0x07));
  return (entryPointsTiming0_ & bit) != 0;
}

bool DivMmc::offTrap(u16 addr) const {
  return (addr & 0xFFF8) == 0x1FF8 && (entryPoints1_ & 0x40) != 0;
}

bool DivMmc::ioRead(u16 port, u8& value) const {
  if (u8(port & 0xFF) != PortControl) return false;
  value = control_;
  return true;
}

bool DivMmc::ioWrite(u16 port, u8 value) {
  if (u8(port & 0xFF) != PortControl) return false;
  // The Next can clear the MAPRAM bit through NextReg 0x09 bit 3; nothing here
  // watches that, so the latch is simply writable.
  control_ = value;
  return true;
}

bool DivMmc::read(u16 addr, bool m1, u8 a, u8& value) {
  (void)a;
  if (rom_.empty() || addr >= 0x4000) return false;

  // The automap, as the Next's VHDL has it: a hook fetch sets a flip-flop that
  // keeps the block in, but for a delayed entry point the byte that fetch
  // returns still comes from the machine's own memory -- only the next access
  // sees the divMMC. An instant entry point answers its own fetch.
  if (m1 && automap_ && hookEnabled(addr)) {
    if (hookInstant(addr)) {
      held_ = true;
    } else {
      mapPending_ = true;
    }
  }
  // The trap at 0x1FF8-0x1FFF is the way back out, and it also has to answer
  // itself: the boot ROM jumps there and returns to an address in the ROM the
  // divMMC has just uncovered.
  if (m1 && offTrap(addr)) unmapPending_ = true;

  if (!shown()) return false;

  if (addr < 0x2000) {
    const u32 page = mapram() ? 3u : romPage();
    if (page < romPages()) {
      value = rom_[page * RomSize + addr];
    } else {
      value = ram_[size_t(3) * BankSize + addr];
    }
    return true;
  }
  value = ram_[size_t(bank()) * BankSize + (addr - 0x2000)];
  return true;
}

bool DivMmc::write(u16 addr, u8 value) {
  if (rom_.empty() || addr >= 0x4000 || !shown()) return false;
  if (addr < 0x2000) {
    if (!mapram()) return true;  // the ROM half is read-only, but still ours
    ram_[size_t(3) * BankSize + addr] = value;
    return true;
  }
  ram_[size_t(bank()) * BankSize + (addr - 0x2000)] = value;
  return true;
}

void DivMmc::afterInstruction() {
  if (mapPending_) {
    held_ = true;
    mapPending_ = false;
  }
  if (unmapPending_) {
    held_ = false;
    control_ &= u8(~Conmem);
    unmapPending_ = false;
  }
}

bool DivMmc::loadRom(std::vector<u8> rom, std::string& error) {
  if (rom.empty()) {
    error = "the divMMC ROM image is empty";
    return false;
  }
  if (rom.size() % RomSize != 0) {
    error = "the divMMC ROM must be a multiple of 8K, got " +
            std::to_string(rom.size()) + " bytes";
    return false;
  }
  if (rom.size() > 4 * RomSize) {
    error = "the divMMC ROM must be 8K or 32K, got " + std::to_string(rom.size()) +
            " bytes";
    return false;
  }
  rom_ = std::move(rom);
  control_ &= u8(~Conmem);
  held_ = false;
  mapPending_ = false;
  unmapPending_ = false;
  return true;
}

bool DivMmc::loadRomFile(const std::string& path, std::string& error) {
  std::vector<u8> data;
  if (!readWholeFile(path, data, error)) return false;
  return loadRom(std::move(data), error);
}

} // namespace rainbow
