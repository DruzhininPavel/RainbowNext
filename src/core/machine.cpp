#include "core/machine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#include "formats/sna.h"
#include "formats/snapshot.h"
#include "formats/z80file.h"

namespace mynext {

// ===========================================================================
// Machine descriptions
// ===========================================================================

const Timings& timingsFor(MachineType type) {
  // Values mirror the base_timings table in Fuse's libspectrum (see
  // docs/REFERENCES.md). Fields: cpu, ay, left, screen, right, hretrace,
  // top, vscreen, bottom, vretrace, intlen, top_left_pixel, contended.
  static const Timings t48{3500000, 0, 24, 128, 24, 48, 48, 192, 48, 24, 32, 14336, true};
  static const Timings t128{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 36, 14362, true};
  static const Timings tPlus3{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 32, 14365, true};
  static const Timings tPentagon{3584000, 1792000, 36, 128, 28, 32, 64, 192, 48, 16, 36, 17988, false};
  static const Timings tNext{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 36, 14362, true};

  switch (type) {
    case MachineType::Spectrum48: return t48;
    case MachineType::Spectrum128: return t128;
    case MachineType::Plus2: return t128;
    case MachineType::Plus3: return tPlus3;
    case MachineType::Pentagon: return tPentagon;
    case MachineType::Next: return tNext;
  }
  return t48;
}

const char* machineName(MachineType type) {
  switch (type) {
    case MachineType::Spectrum48: return "ZX Spectrum 48K";
    case MachineType::Spectrum128: return "ZX Spectrum 128K";
    case MachineType::Plus2: return "ZX Spectrum +2";
    case MachineType::Plus3: return "ZX Spectrum +3";
    case MachineType::Pentagon: return "Pentagon 128K";
    case MachineType::Next: return "ZX Spectrum Next";
  }
  return "Unknown";
}

u32 tstatesPerLine(const Timings& t) {
  return u32(t.leftBorder) + t.horizontalScreen + t.rightBorder + t.horizontalRetrace;
}

u32 tstatesPerFrame(const Timings& t) {
  return tstatesPerLine(t) *
         (u32(t.topBorder) + t.verticalScreen + t.bottomBorder + t.verticalRetrace);
}

// ===========================================================================
// Palette (ARGB8888)
// ===========================================================================

const u32 Machine::Palette[PaletteSize] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7,
    0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF,
};

// ===========================================================================
// Construction / reset
// ===========================================================================

Machine::Machine(MachineType type)
    : type_(type), timings_(timingsFor(type)), cpu_(*this) {
  tstatesPerLine_ = tstatesPerLine(timings_);
  tstatesPerFrame_ = tstatesPerFrame(timings_);
  rom_.assign(0x8000, 0);
  ram_.assign(RamBanks * BankSize, 0);
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);
  reset();
}

void Machine::reset() {
  cpu_.reset();
  ula_.reset(type_ != MachineType::Spectrum48);
  keyboard_.reset();
  beeper_.reset();
  tape_.rewind();

  ram_.assign(RamBanks * BankSize, 0);
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);

  ramBank_ = 0;
  screenBank_ = 5;
  romBank_ = 0;
  port7ffd_ = 0;
  pagingLocked_ = false;

  tstates_ = 0;
  frames_ = 0;
  irqLine_ = false;
  irqEnd_ = 0;

  audio_.clear();
  audioAccum_ = 0;
  ayRegs_.fill(0);
  aySelected_ = 0;
  kempston_ = 0;
}

// ===========================================================================
// ROM and snapshots
// ===========================================================================

bool Machine::loadRomData(const u8* data, size_t size, int bank) {
  if (data == nullptr || bank < 0 || bank > 1) return false;
  const size_t copy = std::min(size, size_t(BankSize));
  if (copy == 0) return false;
  std::memcpy(rom_.data() + size_t(bank) * BankSize, data, copy);
  romLoaded_ = true;
  return true;
}

bool Machine::loadTape(const std::string& path, std::string& error) {
  return tape_.load(path, error);
}

// Serves the ROM's LD-BYTES routine (0x0556) from the loaded image instead of
// waiting for the real pulse train. On entry A holds the flag byte the caller
// expects, IX the destination and DE the length; on success the routine would
// leave with DE = 0 and carry set, which the exit at 0x05DF reproduces by
// loading A from a zero checksum and comparing it against 1.
bool Machine::applyTapeFastLoad() {
  const formats::TapeBlock* block = tape_.seekDataBlock();
  if (block == nullptr || block->data.empty()) return false;

  Z80::Registers& regs = cpu_.regs();
  if (block->data[0] != regs.a) return false; // misaligned tape: let the ROM wait

  const size_t length = size_t(regs.d) << 8 | regs.e;
  const size_t available = block->data.size() - 1;
  const size_t count = std::min(length, available);
  for (size_t i = 0; i < count; ++i) memWrite(u16(regs.ix + i), block->data[1 + i], 0);

  tape_.stepBlock();
  regs.d = 0;
  regs.e = 0;
  regs.ix = u16(regs.ix + count);
  // The loader ends with `LD A,H / XOR L / LD H,A / CP 1`: success is carry,
  // so the two checksum accumulators must xor to zero.
  regs.h = 0;
  regs.l = 0;
  regs.a = 0;
  regs.pc = 0x05DF;
  return true;
}

bool Machine::loadRom(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return loadRomData(data.data(), data.size(), 0);
}

bool Machine::loadRoms(const std::string& rom0, const std::string& rom1) {
  bool ok = loadRom(rom0);
  std::ifstream in(rom1, std::ios::binary);
  if (in) {
    std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = loadRomData(data.data(), data.size(), 1) && ok;
  }
  return ok;
}

bool Machine::loadSnapshot(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  std::string ext;
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    ext = path.substr(dot);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  }

  formats::Snapshot snapshot;
  bool ok = false;
  if (ext == ".sna") ok = formats::loadSna(data, snapshot, error);
  else if (ext == ".z80") ok = formats::loadZ80(data, snapshot, error);
  else {
    error = "unsupported snapshot format: " + ext;
    return false;
  }

  if (!ok) return false;
  applySnapshot(snapshot);
  return true;
}

void Machine::applySnapshot(const formats::Snapshot& snapshot) {
  cpu_.regs() = snapshot.regs;
  ram_ = snapshot.ram;
  if (ram_.size() != size_t(RamBanks * BankSize)) ram_.resize(RamBanks * BankSize, 0);

  pagingLocked_ = false;
  port7ffd_ = snapshot.port7ffd;
  ramBank_ = u8(snapshot.port7ffd & 0x07);
  screenBank_ = (snapshot.port7ffd & 0x08) ? 7 : 5;
  romBank_ = (snapshot.port7ffd & 0x10) ? 1 : 0;

  ula_.write(0xFE, u8(snapshot.border & 0x07));
  beeper_.setLevel(ula_.beeper());
}

// ===========================================================================
// Execution
// ===========================================================================

void Machine::stepInstruction() {
  if (irqLine_) cpu_.interrupt();
  if (cpu_.regs().pc == 0x0556 && tape_.fastLoad() && applyTapeFastLoad()) return;
  cpu_.step();
}

void Machine::runFrame() {
  const u64 target = frames_ + 1;
  while (frames_ < target) stepInstruction();
}

void Machine::runCycles(u32 cycles) {
  const u64 target = tstates_ + cycles;
  while (tstates_ < target) stepInstruction();
}

// ===========================================================================
// Memory
// ===========================================================================

u8 Machine::memRead(u16 addr, u32) {
  if (addr < 0x4000) return rom_[size_t(romBank_) * BankSize + addr];
  if (addr < 0x8000) return ram_[size_t(5) * BankSize + (addr - 0x4000)];
  if (addr < 0xC000) return ram_[size_t(2) * BankSize + (addr - 0x8000)];
  return ram_[size_t(ramBank_) * BankSize + (addr - 0xC000)];
}

void Machine::memWrite(u16 addr, u8 value, u32) {
  if (addr < 0x4000) return; // ROM
  if (addr < 0x8000) {
    ram_[size_t(5) * BankSize + (addr - 0x4000)] = value;
  } else if (addr < 0xC000) {
    ram_[size_t(2) * BankSize + (addr - 0x8000)] = value;
  } else {
    ram_[size_t(ramBank_) * BankSize + (addr - 0xC000)] = value;
  }
}

// ===========================================================================
// I/O
// ===========================================================================

u8 Machine::ioRead(u16 port, u32) {
  if ((port & 0x0001) == 0) {
    if ((port & 0x00FF) == 0xFE) {
      u8 value = ula_.read(port, keyboard_);
      value = u8((value & ~0x40) | (tape_.ear() ? 0x40 : 0x00));
      return value;
    }
    return floatingBus();
  }

  if (type_ != MachineType::Spectrum48) {
    if ((port & 0xC002) == 0x8000) return readAy(port); // 0xBFFD data
    if ((port & 0xC002) == 0xC000) return 0xFF;         // 0xFFFD status
  }

  if ((port & 0x00E0) == 0x0000) return kempston_;      // 0x1F
  return 0xFF;
}

void Machine::ioWrite(u16 port, u8 value, u32) {
  if ((port & 0x0001) == 0) {
    ula_.write(port, value);
    beeper_.setLevel(ula_.beeper());
    return;
  }

  if (type_ != MachineType::Spectrum48) {
    if ((port & 0x8002) == 0x0000) { // 0x7FFD
      writePort7ffd(value);
      return;
    }
    if ((port & 0xC002) == 0xC000) { // 0xFFFD
      aySelected_ = u8(value & 0x0F);
      return;
    }
    if ((port & 0xC002) == 0x8000) { // 0xBFFD
      writeAy(port, value);
      return;
    }
  }
}

void Machine::writePort7ffd(u8 value) {
  if (pagingLocked_) return;
  port7ffd_ = value;
  ramBank_ = u8(value & 0x07);
  screenBank_ = (value & 0x08) ? 7 : 5;
  romBank_ = (value & 0x10) ? 1 : 0;
  pagingLocked_ = (value & 0x20) != 0;
}

u8 Machine::readAy(u16) const {
  return ayRegs_[size_t(aySelected_ & 0x0F)];
}

void Machine::writeAy(u16, u8 value) {
  ayRegs_[size_t(aySelected_ & 0x0F)] = value;
}

// ===========================================================================
// Contention and floating bus
// ===========================================================================

u8 Machine::contention(u16 addr, u32 t) {
  if (!timings_.contended) return 0;

  const bool inScreenBank = (addr >= 0x4000 && addr < 0x8000);
  const bool inPagedScreen = (addr >= 0xC000 && ramBank_ == screenBank_);
  if (!inScreenBank && !inPagedScreen) return 0;

  const u32 line = t / tstatesPerLine_;
  const u32 hc = t % tstatesPerLine_;
  if (line < timings_.topBorder || line >= u32(timings_.topBorder) + timings_.verticalScreen) {
    return 0;
  }
  if (hc < timings_.leftBorder ||
      hc >= u32(timings_.leftBorder) + timings_.horizontalScreen) {
    return 0;
  }

  // The ULA contends on six of every eight T-states. The classic 48K pattern
  // (6,5,4,3,2,1,0,0) starts at the first visible T-state.
  static constexpr u8 Pattern[8] = {5, 4, 3, 2, 1, 0, 0, 6};
  const u32 x = hc - timings_.leftBorder;
  return Pattern[(x + 7) & 7];
}

u8 Machine::floatingBus() const {
  const u32 frameT = u32(tstates_ % tstatesPerFrame_);
  const u32 line = frameT / tstatesPerLine_;
  const u32 hc = frameT % tstatesPerLine_;

  if (line < timings_.topBorder || line >= u32(timings_.topBorder) + timings_.verticalScreen) {
    return 0xFF;
  }
  if (hc < timings_.leftBorder ||
      hc >= u32(timings_.leftBorder) + timings_.horizontalScreen) {
    return 0xFF;
  }

  const u32 row = line - timings_.topBorder;
  const u32 x = hc - timings_.leftBorder;
  u32 column = (x / 8) * 2;
  const u32 sub = x % 8;
  bool attribute = false;
  if (sub == 3) {
    attribute = true;
  } else if (sub == 5) {
    ++column;
    attribute = true;
  } else if (sub >= 4) {
    ++column;
  }
  if (column >= 32) return 0xFF;

  const size_t bankBase = size_t(screenBank_) * BankSize;
  if (attribute) {
    return ram_[bankBase + 0x1800 + size_t(row >> 3) * 32 + column];
  }
  const size_t pixelBase = bankBase + ((row & 0xC0) << 5) + ((row & 0x07) << 8) + ((row & 0x38) << 2);
  return ram_[pixelBase + column];
}

// ===========================================================================
// Clock, audio and video
// ===========================================================================

void Machine::setSampleRate(int hz) {
  if (hz > 0) sampleRate_ = hz;
}

void Machine::tick(u32 cycles) {
  u32 remaining = cycles;
  while (remaining > 0) {
    const u32 frameT = u32(tstates_ % tstatesPerFrame_);
    const u32 hc = frameT % tstatesPerLine_;
    const u32 step = std::min(remaining, tstatesPerLine_ - hc);

    tstates_ += step;
    remaining -= step;
    tape_.advance(step);

    audioAccum_ += u64(step) * u64(sampleRate_);
    while (audioAccum_ >= u64(timings_.cpuHz)) {
      audioAccum_ -= timings_.cpuHz;
      audio_.push_back(beeper_.sample());
    }
    if (audio_.size() > size_t(sampleRate_) * 2) {
      audio_.erase(audio_.begin(), audio_.begin() + long(audio_.size() - size_t(sampleRate_)));
    }

    const u32 newFrameT = u32(tstates_ % tstatesPerFrame_);
    if (newFrameT % tstatesPerLine_ == 0) {
      const u32 linesPerFrame = tstatesPerFrame_ / tstatesPerLine_;
      const u32 completed = (newFrameT == 0) ? (linesPerFrame - 1)
                                             : (newFrameT / tstatesPerLine_ - 1);
      if (completed < borderLines_.size()) borderLines_[completed] = ula_.border();

      if (newFrameT == 0) {
        renderFrame();
        ++frames_;
        frameStart();
      }
    }

    if (irqLine_ && newFrameT >= irqEnd_) irqLine_ = false;
  }
}

void Machine::frameStart() {
  irqLine_ = true;
  irqEnd_ = timings_.interruptLength;
}

void Machine::renderFrame() {
  for (int r = 0; r < FrameHeight; ++r) {
    if (r < BorderHeight || r >= BorderHeight + ScreenHeight) {
      int line = 0;
      if (r < BorderHeight) {
        line = r;
      } else {
        line = timings_.topBorder + ScreenHeight + (r - BorderHeight - ScreenHeight);
      }
      if (line < 0 || line >= int(borderLines_.size())) line = 0;
      const u32 color = Palette[borderLines_[size_t(line)] & 0x07];
      u32* row = framebuffer_.data() + size_t(r) * FrameWidth;
      for (int x = 0; x < FrameWidth; ++x) row[x] = color;
    } else {
      renderScreenRow(r - BorderHeight);
    }
  }
}

void Machine::renderScreenRow(int y) {
  const int outY = BorderHeight + y;
  const u8 borderColor = borderLines_[size_t(timings_.topBorder + y)] & 0x07;
  u32* row = framebuffer_.data() + size_t(outY) * FrameWidth;

  for (int x = 0; x < BorderWidth; ++x) row[x] = Palette[borderColor];
  for (int x = FrameWidth - BorderWidth; x < FrameWidth; ++x) row[x] = Palette[borderColor];

  const size_t bankBase = size_t(screenBank_) * BankSize;
  const size_t pixelBase = bankBase + ((size_t(y) & 0xC0) << 5) +
                           ((size_t(y) & 0x07) << 8) + ((size_t(y) & 0x38) << 2);
  const size_t attrBase = bankBase + 0x1800 + size_t(y >> 3) * 32;
  const bool flashOn = ((frames_ >> 4) & 1) != 0;

  for (int col = 0; col < 32; ++col) {
    const u8 pixels = ram_[pixelBase + size_t(col)];
    const u8 attr = ram_[attrBase + size_t(col)];
    const u8 bright = u8(attr & 0x40) ? 8 : 0;
    const u8 ink = u8((attr & 0x07) | bright);
    const u8 paper = u8(((attr >> 3) & 0x07) | bright);
    const bool flash = (attr & 0x80) != 0 && flashOn;
    const u8 inkColor = flash ? paper : ink;
    const u8 paperColor = flash ? ink : paper;

    u32* dest = row + BorderWidth + col * 8;
    for (int b = 0; b < 8; ++b) {
      dest[b] = Palette[(pixels & (0x80u >> b)) ? inkColor : paperColor];
    }
  }
}

} // namespace mynext
