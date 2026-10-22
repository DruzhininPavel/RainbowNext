#include "peripherals/divmmc.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace mynext {

namespace {

// A template for the card's CSD, taken from the shape a real 4 GB SDHC reports:
// CSD version 2.0, 512-byte blocks, SDHC addressing. Only C_SIZE is rewritten
// to match the image, because that is the one field a driver sanity-checks.
constexpr u8 kCsdTemplate[16] = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00,
                                 0x1D, 0x3A, 0x7F, 0x80, 0x0A, 0x40, 0x00, 0x01};

// Card identification: manufacturer 0x03, OEM "SD", product "myNext", and a
// serial. Nothing reads it beyond printing it, but a driver that asks for it
// should not get garbage.
constexpr u8 kCid[16] = {0x03, 'S', 'D', 'm', 'y', 'N', 'e', 'x',
                         0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01};

// Commands the module answers. Everything else gets an illegal-command
// response, which is what a real card does and what tells a driver to stop.
enum : u8 {
  CmdGoIdle = 0,
  CmdSendCid = 10,
  CmdSendCsd = 9,
  CmdSendIfCond = 8,
  CmdSendOpCond = 1,
  CmdSendStatus = 13,
  CmdSetBlockLen = 16,
  CmdReadSingle = 17,
  CmdWriteBlock = 24,
  CmdAppCmd = 55,
  CmdReadOcr = 58,
  CmdCrcOnOff = 59,
  CmdSendOpCondA = 41,  // ACMD41, only valid after CMD55
};

constexpr u8 R1Idle = 0x01;
constexpr u8 R1Ready = 0x00;
constexpr u8 R1Illegal = 0x04;
constexpr u8 R1ParameterError = 0x40;
constexpr u8 DataToken = 0xFE;

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
// SdCard
// ===========================================================================

bool SdCard::load(std::vector<u8> image, std::string& error) {
  if (image.empty()) {
    error = "the card image is empty";
    return false;
  }
  if (image.size() % 512 != 0) {
    error = "the card image is not a whole number of 512-byte sectors";
    return false;
  }
  image_ = std::move(image);
  reset();
  return true;
}

void SdCard::eject() {
  image_.clear();
  reset();
}

void SdCard::reset() {
  selected_ = false;
  state_ = State::Command;
  nextState_ = State::Command;
  cmd_ = 0;
  arg_ = 0;
  frameBytes_ = 0;
  appCmd_ = false;
  idle_ = true;
  sdhc_ = false;
  out_.clear();
  outPos_ = 0;
  tokenSeen_ = false;
  writeAddr_ = 0;
  writeBuf_.clear();
}

void SdCard::select(bool asserted) {
  // Dropping chip select abandons whatever was in flight, exactly like a real
  // card: a driver that gives up mid-transfer must not leave it half-written.
  if (selected_ && !asserted) {
    state_ = State::Command;
    nextState_ = State::Command;
    frameBytes_ = 0;
    tokenSeen_ = false;
    writeBuf_.clear();
    out_.clear();
    outPos_ = 0;
  }
  selected_ = asserted;
}

void SdCard::queue(const std::vector<u8>& bytes) {
  out_ = bytes;
  outPos_ = 0;
}

u8 SdCard::transfer(u8 mosi) {
  if (!selected_ || image_.empty()) return 0xFF;

  switch (state_) {
    case State::Command: {
      if (frameBytes_ == 0) {
        // A command frame starts with 0x40 | cmd. Anything else is one of the
        // 0xFF idle bytes a driver sends to give the card clock to wake up.
        if ((mosi & 0xC0) != 0x40) return 0xFF;
        cmd_ = u8(mosi & 0x3F);
        arg_ = 0;
        frameBytes_ = 1;
        return 0xFF;
      }
      if (frameBytes_ <= 4) {
        arg_ = (arg_ << 8) | mosi;
        ++frameBytes_;
        return 0xFF;
      }
      ++frameBytes_;  // the CRC byte, which SPI mode ignores
      execute();
      state_ = State::Responding;
      return nextByte();
    }
    case State::Responding:
    case State::Writing:
      return nextByte();
    case State::Receiving:
      return receiveWrite(mosi);
  }
  return 0xFF;
}

u8 SdCard::nextByte() {
  if (outPos_ < out_.size()) return out_[outPos_++];
  out_.clear();
  outPos_ = 0;
  if (state_ == State::Responding || state_ == State::Writing) {
    state_ = nextState_;
    nextState_ = State::Command;
    frameBytes_ = 0;
  }
  return 0xFF;
}

void SdCard::execute() {
  nextState_ = State::Command;

  if (cmd_ != CmdAppCmd) {
    const bool wasApp = appCmd_;
    appCmd_ = false;
    if (cmd_ == CmdSendOpCondA && wasApp) {
      // ACMD41 is the initialisation handshake: the HCS bit asks for block
      // addressing, which is what makes the card an SDHC.
      sdhc_ = (arg_ & 0x40000000u) != 0;
      idle_ = false;
      queue({R1Ready});
      return;
    }
    if (cmd_ == CmdSendOpCondA) {
      queue({R1Illegal});  // ACMD41 without the CMD55 prefix
      return;
    }
  }

  switch (cmd_) {
    case CmdGoIdle:
      idle_ = true;
      sdhc_ = false;
      queue({R1Idle});
      break;
    case CmdSendIfCond:
      // R7: the voltage accepted, so a driver knows the card speaks 2.7-3.6 V.
      queue({R1Idle, 0x00, 0x00, 0x01, 0xAA});
      break;
    case CmdAppCmd:
      appCmd_ = true;
      queue({R1Idle});
      break;
    case CmdSendOpCond:
      queue({R1Idle});  // the MMC-era form; SD cards keep asking for ACMD41
      break;
    case CmdReadOcr:
      queue({u8(idle_ ? R1Idle : R1Ready), 0x40, u8(sdhc_ ? 0xFF : 0x80), 0x80,
             0x00});
      break;
    case CmdSendCsd:
      queueCsd();
      break;
    case CmdSendCid:
      queueCid();
      break;
    case CmdSetBlockLen:
      queue({R1Ready});
      break;
    case CmdCrcOnOff:
      queue({R1Ready});
      break;
    case CmdSendStatus:
      queue({u8(idle_ ? R1Idle : R1Ready), 0x00});
      break;
    case CmdReadSingle: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      std::vector<u8> block;
      block.reserve(3 + 512 + 2);
      block.push_back(R1Ready);
      block.push_back(0xFF);  // one byte of latency before the token
      block.push_back(DataToken);
      block.insert(block.end(), image_.begin() + addr, image_.begin() + addr + 512);
      block.push_back(0xFF);
      block.push_back(0xFF);
      queue(block);
      break;
    }
    case CmdWriteBlock: {
      const u32 addr = address();
      if (!inRange(addr)) {
        queue({R1ParameterError});
        break;
      }
      writeAddr_ = addr;
      queue({R1Ready});
      nextState_ = State::Receiving;
      break;
    }
    default:
      queue({R1Illegal});
      break;
  }
}

u8 SdCard::receiveWrite(u8 mosi) {
  if (!tokenSeen_) {
    if (mosi != DataToken) return 0xFF;  // still waiting for the data token
    tokenSeen_ = true;
    writeBuf_.clear();
    return 0xFF;
  }
  writeBuf_.push_back(mosi);
  if (writeBuf_.size() < 512u + 2u) return 0xFF;  // payload plus two CRC bytes

  if (inRange(writeAddr_)) {
    std::copy(writeBuf_.begin(), writeBuf_.begin() + 512, image_.begin() + writeAddr_);
  }
  tokenSeen_ = false;
  writeBuf_.clear();
  // The data-response token: accepted, then a couple of busy bytes and the card
  // is ready again. The write stays in the in-memory image.
  queue({0x05, 0xFF, 0x00});
  state_ = State::Writing;
  nextState_ = State::Command;
  return nextByte();
}

void SdCard::queueCsd() {
  u8 csd[16];
  std::copy(std::begin(kCsdTemplate), std::end(kCsdTemplate), csd);
  // C_SIZE counts 512 KB units, so it is sectors / 1024 - 1.
  const u32 csize = sectors() >= 1024 ? sectors() / 1024 - 1 : 0;
  csd[7] = u8((csize >> 16) & 0x3F);
  csd[8] = u8((csize >> 8) & 0xFF);
  csd[9] = u8(csize & 0xFF);

  std::vector<u8> block;
  block.reserve(3 + 16 + 2);
  block.push_back(R1Ready);
  block.push_back(0xFF);
  block.push_back(DataToken);
  block.insert(block.end(), std::begin(csd), std::end(csd));
  block.push_back(0xFF);
  block.push_back(0xFF);
  queue(block);
}

void SdCard::queueCid() {
  std::vector<u8> block;
  block.reserve(3 + 16 + 2);
  block.push_back(R1Ready);
  block.push_back(0xFF);
  block.push_back(DataToken);
  block.insert(block.end(), std::begin(kCid), std::end(kCid));
  block.push_back(0xFF);
  block.push_back(0xFF);
  queue(block);
}

// ===========================================================================
// DivMmc
// ===========================================================================

const PeripheralInfo& DivMmc::info() const {
  static const PeripheralInfo kInfo = [] {
    PeripheralInfo info;
    info.id = "divmmc";
    info.name = "DivMMC + SD card";
    info.version = "1.0";
    info.author = "myNext";
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
  if (low == PortSpiData || low == PortUnmap || low == PortMapIn) {
    // The DivMMC+ hands the SPI byte back on 0xE7 and 0xEB as well, so a driver
    // written for either board works.
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
  // The card's chip select is not a port of its own: the board ties it to the
  // mapping latch, so a driver talks to the card exactly while the module is
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

bool DivMmc::memRead(u16 addr, u32, bool m1, u8& value) {
  if (rom_.empty() || addr >= 0x4000) return false;

  // The automap: an opcode fetch at a hook address maps the module in before
  // the fetch is answered, which is the whole point of the M1 flag.
  if (m1 && isHookAddress(addr)) control_ |= Conmem;
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

} // namespace mynext
