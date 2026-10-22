#include "peripherals/divmmc.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace rainbow {

namespace {

std::string lowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return ext;
}

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

// ===========================================================================
// DivMmc
// ===========================================================================

const PeripheralInfo& DivMmc::info() const {
  static const PeripheralInfo kInfo = [] {
    PeripheralInfo info;
    info.id = "divmmc";
    info.name = "DivMMC + SD card";
    info.version = "1.0";
    info.author = "RainbowNext";
    info.description =
        "esxDOS mass storage: an 8K DivMMC ROM and a 128K RAM window that map "
        "over 0x0000-0x3FFF, an automap on the hook addresses, and an SD card "
        "on the SPI ports. Load an esxDOS ROM and a card image, then enable "
        "it. The status port shares 0x1F with the Kempston joystick.";
    info.ports = {PortControl, PortUnmap, PortMapIn, PortSpiData, PortStatus};
    info.nextOnly = false;
    return info;
  }();
  return kInfo;
}

bool DivMmc::isHookAddress(u16 addr) {
  switch (addr) {
    case 0x0000:  // reset
    case 0x0008:  // RST 8
    case 0x0038:  // IM 1 interrupt
    case 0x0066:  // NMI
    case 0x04C6:  // esxDOS
    case 0x0562:  // esxDOS
      return true;
    default:
      return false;
  }
}

void DivMmc::reset() {
  control_ = 0;
  spiLatch_ = 0xFF;
  std::fill(ram_.begin(), ram_.end(), u8(0));
  card_.reset();
  card_.select(true);
}

void DivMmc::enable() {
  control_ = 0;
  spiLatch_ = 0xFF;
  card_.select(true);
}

void DivMmc::disable() {
  control_ = 0;
  spiLatch_ = 0xFF;
  card_.select(false);
}

u32 DivMmc::romPage() const {
  // A single 8K image has one page. A 32K image carries the four esxDOS pages
  // and the top two bits of the latch pick one, which is how a DivMMC+ with a
  // bigger flash behaves.
  if (romPages() <= 1) return 0;
  return u32((control_ >> 4) & 0x03) % romPages();
}

bool DivMmc::ioRead(u16 port, u32, u8& value) {
  const u8 low = u8(port & 0xFF);
  if (low == PortControl) {
    value = control_;
    return true;
  }
  if (low == PortSpiData && legacySpiPort_) {
    value = spiLatch_;
    return true;
  }
  if (low == PortData) {
    // 0xEB is the data port on the Next. On the older boards it is the map-in
    // latch, which still reads back the last byte shifted, so both work.
    value = spiLatch_;
    return true;
  }
  if (low == PortUnmap && legacySpiPort_) {
    value = spiLatch_;
    return true;
  }
  if (low == PortStatus || low == 0x3F || low == 0x9F || low == 0xBF) {
    // Bit 7 is card detect (low when a card is in), bit 6 is busy. The transfer
    // is instantaneous here, so busy is never asserted and the rest floats
    // high, which is what a driver expects from an idle interface.
    value = u8((card_.inserted() ? 0x00 : 0x80) | 0x3F);
    return true;
  }
  return false;
}

bool DivMmc::ioWrite(u16 port, u8 value, u32) {
  const u8 low = u8(port & 0xFF);
  if (low == PortControl) {
    // Bit 6 can only be set on the original board, and only a power cycle
    // clears it. The Next can clear it through NextReg 0x09 bit 3, which this
    // module does not watch, so the latch is simply writable.
    control_ = value;
    card_.select(!mappedIn());
    return true;
  }
  if (!legacySpiPort_) {
    // On the Next the chip select has a port of its own and 0xEB shifts a byte
    // through the card: bit 0 low selects the SD card, high releases it.
    if (low == PortUnmap) {
      card_.select((value & 0x01) != 0);
      return true;
    }
    if (low == PortData) {
      spiLatch_ = card_.transfer(value);
      return true;
    }
    return false;
  }
  // On the older boards the chip select is not a port of its own: it is tied to
  // the mapping latch, so a driver talks to the card exactly while the module is
  // mapped out. That is why esxDOS unmaps before every SPI access.
  if (low == PortUnmap) {
    control_ &= u8(~Conmem);
    card_.select(true);
    return true;
  }
  if (low == PortMapIn) {
    control_ |= Conmem;
    card_.select(false);
    return true;
  }
  if (low == PortSpiData) {
    spiLatch_ = card_.transfer(value);
    return true;
  }
  return false;
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
  // the point fires only while the +3 DOS ROM is paged, which is how the
  // original divMMC behaved and why RST 8 is not hooked on a stock Next.
  if (addr < 0x0066 && (entryPointsValid0_ & bit) == 0 && rom3Present_ &&
      !rom3Present_()) {
    return false;
  }
  return true;
}

bool DivMmc::memRead(u16 addr, u32, bool m1, u8& value) {
  if (rom_.empty() || addr >= 0x4000) return false;

  // The automap: an opcode fetch at a hook address maps the module in before
  // the fetch is answered, which is the whole point of the M1 flag. It only
  // happens while the board option is on.
  if (m1 && automap_ && hookEnabled(addr)) control_ |= Conmem;
  if (!mappedIn()) return false;

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

bool DivMmc::memWrite(u16 addr, u8 value, u32) {
  if (rom_.empty() || addr >= 0x4000 || !mappedIn()) return false;
  if (addr < 0x2000) {
    if (!mapram()) return true;  // the ROM half is read-only, but still ours
    ram_[size_t(3) * BankSize + addr] = value;
    return true;
  }
  ram_[size_t(bank()) * BankSize + (addr - 0x2000)] = value;
  return true;
}

const char* DivMmc::fileFilter() const { return "*.rom;*.mmc;*.img;*.iso;*.bin"; }

bool DivMmc::loadFile(const std::string& path, std::string& error) {
  // One entry point, two kinds of image, decided by the extension: a `.rom` is
  // the esxDOS ROM, everything else is a sector image for the card.
  std::vector<u8> data;
  if (!readWholeFile(path, data, error)) return false;
  if (lowerExtension(path) == "rom") return loadRom(std::move(data), error);
  return loadCard(std::move(data), error);
}

bool DivMmc::loadRom(std::vector<u8> rom, std::string& error) {
  if (rom.empty()) {
    error = "the ROM image is empty";
    return false;
  }
  if (rom.size() % RomSize != 0) {
    error = "the DivMMC ROM must be a multiple of 8K, got " +
            std::to_string(rom.size()) + " bytes";
    return false;
  }
  if (rom.size() > 4 * RomSize) {
    error = "the DivMMC ROM must be 8K or 32K, got " + std::to_string(rom.size()) +
            " bytes";
    return false;
  }
  rom_ = std::move(rom);
  control_ &= u8(~Conmem);
  return true;
}

bool DivMmc::loadCard(std::vector<u8> image, std::string& error) {
  return card_.load(std::move(image), error);
}

bool DivMmc::loadRomFile(const std::string& path, std::string& error) {
  std::vector<u8> data;
  if (!readWholeFile(path, data, error)) return false;
  return loadRom(std::move(data), error);
}

bool DivMmc::loadCardFile(const std::string& path, std::string& error) {
  std::vector<u8> data;
  if (!readWholeFile(path, data, error)) return false;
  return loadCard(std::move(data), error);
}

} // namespace rainbow
