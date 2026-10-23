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
  unmapPending_ = false;
  std::fill(ram_.begin(), ram_.end(), u8(0));
}

void DivMmc::softReset() {
  control_ = 0;
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
    case 0x0066: points = entryPoints1_; bit = 0x01; break;
    case 0x04C6: points = entryPoints1_; bit = 0x04; break;
    case 0x04D7: points = entryPoints1_; bit = 0x10; break;
    case 0x0562: points = entryPoints1_; bit = 0x08; break;
    case 0x056A: points = entryPoints1_; bit = 0x20; break;
    default: return false;
  }
  if ((points & bit) == 0) return false;
  // 0xB9 only covers the eight RST and reset vectors. A clear bit there means
  // the point fires only while the +3 DOS ROM is paged, which is why RST 8 is
  // not hooked on a stock Next.
  if (addr < 0x0066 && (entryPointsValid0_ & bit) == 0 && !rom3Present_) return false;
  return true;
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
  if (rom_.empty() || addr >= 0x4000) return false;

  // The automap: an opcode fetch at a hook address maps the block in before the
  // fetch is answered, which is the whole point of the M1 flag. It only happens
  // while the board option is on.
  if (m1 && automap_ && hookEnabled(addr)) control_ |= Conmem;
  if (!mappedIn()) return false;

  // The esxDOS exit stubs sit at 0x1FF8-0x1FFF and unmap on the way out, but
  // only after the instruction has run, so the stub itself still comes from the
  // divMMC ROM.
  if (m1 && addr >= 0x1FF8 && a == 0x83) unmapPending_ = true;

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
  if (rom_.empty() || addr >= 0x4000 || !mappedIn()) return false;
  if (addr < 0x2000) {
    if (!mapram()) return true;  // the ROM half is read-only, but still ours
    ram_[size_t(3) * BankSize + addr] = value;
    return true;
  }
  ram_[size_t(bank()) * BankSize + (addr - 0x2000)] = value;
  return true;
}

void DivMmc::afterInstruction() {
  if (!unmapPending_) return;
  control_ &= u8(~Conmem);
  unmapPending_ = false;
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
  return true;
}

bool DivMmc::loadRomFile(const std::string& path, std::string& error) {
  std::vector<u8> data;
  if (!readWholeFile(path, data, error)) return false;
  return loadRom(std::move(data), error);
}

} // namespace rainbow
