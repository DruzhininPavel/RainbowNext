#include "core/machine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#include "formats/dsk.h"
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
  rom_.assign(RomPages * PageSize, 0);
  ram_.assign(RamPages * PageSize, 0);
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);
  reset();
}

void Machine::reset() {
  cpu_.reset();
  cpu_.setExtendedInstructions(type_ == MachineType::Next);
  ula_.reset(type_ != MachineType::Spectrum48);
  keyboard_.reset();
  beeper_.reset();
  tape_.rewind();

  ram_.assign(RamPages * PageSize, 0);
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);

  next_.reset();
  palette_.reset();
  sprites_.reset();
  layer2Clip_ = {0, 255, 0, 191};
  layer2ClipIndex_ = 0;
  tilemapClip_ = {0, 159, 0, 255};
  tilemapClipIndex_ = 0;
  occluders_.assign(FrameWidth * FrameHeight, 0);
  spriteDrawn_.assign(FrameWidth * FrameHeight, 0);
  // Bits 6:4 are the display timing and bits 2:0 the machine type; a Next in
  // Next mode presents itself as a +2A/+3.
  if (type_ == MachineType::Next) next_.write(0x03, 0x33);
  ramBank_ = 0;
  screenBank_ = 5;
  romBank_ = 0;
  port7ffd_ = 0;
  port1ffd_ = 0;
  pagingLocked_ = false;
  specialPaging_ = false;
  updatePaging();
  ay_.reset();
  ay_.setStereo(type_ == MachineType::Plus3 ? Ay38912::Stereo::ABC : Ay38912::Stereo::Mono);
  fdc_.reset();
  fdc_.setDisk(hasDiskController() && !disk_.empty() ? &disk_ : nullptr);

  tstates_ = 0;
  cpuTstates_ = 0;
  turboAccum_ = 0;
  frames_ = 0;
  irqLine_ = false;
  irqEnd_ = 0;

  audio_.clear();
  audioAccum_ = 0;
  dcInL_ = dcOutL_ = dcInR_ = dcOutR_ = 0.0f;
  kempston_ = 0;
}

// ===========================================================================
// ROM and snapshots
// ===========================================================================

bool Machine::loadRomData(const u8* data, size_t size, int bank) {
  if (data == nullptr || bank < 0 || bank >= RomBanks) return false;
  const size_t copy = std::min(size, size_t(BankSize));
  if (copy == 0) return false;
  std::memcpy(rom_.data() + size_t(bank) * BankSize, data, copy);
  romLoaded_ = true;
  return true;
}

bool Machine::loadTape(const std::string& path, std::string& error) {
  return tape_.load(path, error);
}

bool Machine::loadDisk(const std::string& path, std::string& error) {
  if (!hasDiskController()) {
    error = "only the +3 has a disk controller";
    return false;
  }
  formats::DiskImage image;
  if (!formats::loadDskFile(path, image, error)) return false;
  disk_ = std::move(image);
  fdc_.reset();
  fdc_.setDisk(&disk_);
  return true;
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
  return loadRomImage(path);
}

// A ROM set is one file per 16K bank: 16K (48K BASIC), 32K (128K: editor +
// 48K BASIC), 48K or 64K (+2A/+3: 128K editor, 48K BASIC, 128K syntax, 48K
// syntax). Any multiple of 16K up to 64K is accepted.
bool Machine::loadRomImage(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.empty()) return false;
  return loadRomImageData(data.data(), data.size());
}

// Splits a ROM image into 16 KiB banks: a 16 KiB file fills bank 0, a 64 KiB
// file fills all four.
bool Machine::loadRomImageData(const u8* data, size_t size) {
  if (data == nullptr || size == 0) return false;

  const int banks = int(std::min<size_t>(size / BankSize, size_t(RomBanks)));
  if (banks == 0) {
    return loadRomData(data, size, 0);
  }
  for (int bank = 0; bank < banks; ++bank) {
    loadRomData(data + size_t(bank) * BankSize, BankSize, bank);
  }
  return true;
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
  std::fill(ram_.begin(), ram_.end(), 0);
  const size_t ramCopy = std::min(snapshot.ram.size(), ram_.size());
  std::copy_n(snapshot.ram.begin(), ramCopy, ram_.begin());

  port7ffd_ = snapshot.port7ffd;
  port1ffd_ = 0;
  pagingLocked_ = false;
  updatePaging();

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
  const u64 target = cpuTstates_ + cycles;
  while (cpuTstates_ < target) stepInstruction();
}

// ===========================================================================
// Memory
// ===========================================================================

// The +2A/+3 "special" paging mode replaces the whole 64K with RAM banks 0-3,
// which is what CP/M runs in. Every other machine keeps bank 5 at 0x4000 and
// bank 2 at 0x8000, with 0xC000 switchable.
u8 Machine::memRead(u16 addr, u32) {
  const Page& page = page_[addr >> 13];
  const size_t offset = size_t(page.page) * PageSize + (addr & (PageSize - 1));
  return page.ram ? ram_[offset] : rom_[offset];
}

void Machine::memWrite(u16 addr, u8 value, u32) {
  const Page& page = page_[addr >> 13];
  if (!page.ram) return;
  ram_[size_t(page.page) * PageSize + (addr & (PageSize - 1))] = value;
}

// ===========================================================================
// I/O
// ===========================================================================

u8 Machine::ioRead(u16 port, u32) {
  if (isNext()) {
    if (port == 0x243B) return next_.selected();
    if (port == 0x253B) return readNextReg(next_.selected());
    if (port == 0x303B) {
      // The sprite status flags clear themselves as they are read.
      const u8 status = sprites_.readStatus();
      sprites_.clearStatus();
      return status;
    }
  }

  if ((port & 0x0001) == 0) {
    if ((port & 0x00FF) == 0xFE) {
      u8 value = ula_.read(port, keyboard_);
      value = u8((value & ~0x40) | (tape_.ear() ? 0x40 : 0x00));
      return value;
    }
    return floatingBus();
  }

  if (hasAy()) {
    if ((port & 0xC002) == 0x8000) return ay_.read(); // 0xBFFD data
    if ((port & 0xC002) == 0xC000) return 0xFF;       // 0xFFFD status
  }

  if (hasDiskController()) {
    if ((port & 0xF002) == 0x2000) return fdc_.readStatus(); // 0x2FFD
    if ((port & 0xF002) == 0x3000) return fdc_.readData();   // 0x3FFD
  }

  if ((port & 0x00E0) == 0x0000) return kempston_;      // 0x1F
  return 0xFF;
}

void Machine::ioWrite(u16 port, u8 value, u32) {
  if (isNext()) {
    if (port == 0x243B) {
      next_.select(value);
      return;
    }
    if (port == 0x253B) {
      writeNextReg(next_.selected(), value);
      return;
    }
    if (port == 0x303B) {
      sprites_.selectPort(value);
      return;
    }
    if ((port & 0x00FF) == 0x57) { // sprite attributes, streamed
      sprites_.writeAttributePort(value);
      return;
    }
    if ((port & 0x00FF) == 0x5B) { // sprite pattern bytes
      sprites_.writePattern(value);
      return;
    }
  }

  if ((port & 0x0001) == 0) {
    ula_.write(port, value);
    beeper_.setLevel(ula_.beeper());
    return;
  }

  if (is128k()) {
    // The +2A/+3 decodes both paging ports more tightly than the 128K/+2:
    // 0x1FFD needs bit 12 set with 13-15 clear, 0x7FFD needs bit 14 set.
    // That also keeps the two apart, whereas the 128K's loose 0x7FFD mask
    // would swallow a 0x1FFD write.
    if (isPlus3() || isNext()) {
      if ((port & 0xF002) == 0x1000) { // 0x1FFD
        writePort1ffd(value);
        return;
      }
      if ((port & 0xC002) == 0x4000) { // 0x7FFD
        writePort7ffd(value);
        return;
      }
    } else if ((port & 0x8002) == 0x0000) { // 0x7FFD
      writePort7ffd(value);
      return;
    }
    if ((port & 0xC002) == 0xC000) { // 0xFFFD, and 0xFFFF on the +2A/+3
      ay_.select(value);
      return;
    }
    if ((port & 0xC002) == 0x8000) { // 0xBFFD
      ay_.write(value);
      return;
    }
  }

  if (hasDiskController()) {
    if ((port & 0xF002) == 0x2000) { // 0x2FFD, command register
      fdc_.writeCommand(value);
      return;
    }
    if ((port & 0xF002) == 0x3000) { // 0x3FFD, data register
      fdc_.writeData(value);
      return;
    }
  }
}

void Machine::writePort7ffd(u8 value) {
  if (pagingLocked_) return;
  port7ffd_ = value;
  pagingLocked_ = (value & 0x20) != 0;
  // On the Next the classic ports are a shorthand for MMU writes.
  if (isNext() && !pagingLocked_) {
    const u8 bank = u8(value & 0x07);
    next_.setMmuSlot(6, u8(bank * 2));
    next_.setMmuSlot(7, u8(bank * 2 + 1));
  }
  updatePaging();
}

// 0x1FFD exists only on the +2A/+3: bit 0 selects the special (CP/M) paging
// mode, bit 2 is the high ROM bit and bit 3 starts the disk motor. Bit 1 is
// ignored in normal mode and only matters inside CP/M mode, where it picks the
// memory map together with bit 2 -- so it is not a lock. Paging is locked
// exclusively by bit 5 of 0x7FFD, which freezes this port too.
void Machine::writePort1ffd(u8 value) {
  if (pagingLocked_) return;
  port1ffd_ = value;
  updatePaging();
}

void Machine::updatePaging() {
  u8 rom = u8((port7ffd_ & 0x10) ? 1 : 0);
  if (isPlus3() || isNext()) rom = u8(rom | ((port1ffd_ & 0x04) ? 2 : 0));
  romBank_ = u8(rom & 0x03);

  if (isNext()) {
    // The Next keeps its map in the MMU registers, and the classic paging
    // ports only write into those same registers.
    updateNextMmu();
    return;
  }

  specialPaging_ = isPlus3() && (port1ffd_ & 0x01) != 0;
  // Bit 3 of 0x1FFD is the disk motor; nothing in the emulator needs it, but
  // the debugger shows it and it mirrors what the ROM asked for.
  fdc_.setMotor(isPlus3() && (port1ffd_ & 0x08) != 0);

  ramBank_ = u8(port7ffd_ & 0x07);
  screenBank_ = (port7ffd_ & 0x08) ? 7 : 5;

  if (specialPaging_) {
    // +2A/+3 CP/M mode: bits 1 and 2 of 0x1FFD pick one of four fixed maps.
    // Bank 2/6 stays at 0x8000 in every configuration, so only the other
    // three slots change.
    static constexpr u8 Maps[4][4] = {
        {0, 1, 2, 3}, // bit2=0 bit1=0
        {4, 5, 6, 7}, // bit2=0 bit1=1
        {4, 5, 6, 3}, // bit2=1 bit1=0
        {4, 7, 6, 3}, // bit2=1 bit1=1
    };
    const u8 config = u8((port1ffd_ >> 1) & 0x03);
    for (int bank = 0; bank < 4; ++bank) mapBank(bank, Maps[config][bank], true);
    return;
  }

  mapBank(0, romBank_, false);
  mapBank(1, 5, true); // the fixed bank holding the main screen
  mapBank(2, 2, true);
  mapBank(3, ramBank_, true);
}

void Machine::mapBank(int slot, int bank, bool ram) {
  for (int half = 0; half < 2; ++half) {
    Page& page = page_[size_t(slot * 2 + half)];
    page.ram = ram;
    page.page = u16(bank * 2 + half);
  }
}

// The Next's MMU is eight byte-wide registers, one per 8K slot. A value of 255
// shows the ROM instead of RAM, and the ROM half follows the slot's parity.
void Machine::updateNextMmu() {
  // Register 0x8C swaps the two low ROM banks, which is how NextZXOS brings
  // in the 48K ROM; bit 6 keeps the swap to the write half of the bus.
  u8 rom = romBank_;
  if ((next_.read(0x8C) & 0x80) != 0) rom = u8(rom ^ 0x01);

  for (int slot = 0; slot < 8; ++slot) {
    const u8 value = next_.mmuSlot(slot);
    Page& page = page_[size_t(slot)];
    if (value == NextRegFile::RomPage) {
      page.ram = false;
      page.page = u16(rom * 2 + (slot & 1));
    } else {
      page.ram = true;
      page.page = value;
    }
  }
}


// ===========================================================================
// Next register file
// ===========================================================================

u8 Machine::readNextReg(u8 reg) const {
  // The raster counters are the only registers whose value lives in the
  // machine rather than in the file itself.
  if (reg == 0x40) return palette_.index();
  if (reg == 0x1E || reg == 0x1F) {
    const u32 line = u32(tstates_ % tstatesPerFrame_) / tstatesPerLine_;
    return reg == 0x1E ? u8(line) : u8(line >> 8);
  }
  return next_.read(reg);
}

void Machine::writeNextReg(u8 reg, u8 value) {
  switch (reg) {
    case 0x02: // soft or hard reset
      if ((value & 0x03) != 0) reset();
      return;
    case 0x03: {
      // The display timing bits are locked unless bit 7 unlocks them.
      u8 type = next_.read(0x03);
      if ((value & 0x80) != 0) type = u8((type & 0x8F) | (value & 0x70));
      next_.write(0x03, type);
      return;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x8C: // alternate ROM
      next_.write(reg, value);
      updatePaging();
      return;
    case 0x40: // palette index
      next_.write(reg, value);
      palette_.setIndex(value);
      return;
    case 0x41: // 8-bit palette colour
      next_.write(reg, value);
      palette_.write8(value);
      return;
    case 0x43: // palette control
      next_.write(reg, value);
      palette_.setControl(value);
      return;
    case 0x44: // 9-bit palette colour
      palette_.write9(value);
      return;
    case 0x09: // peripheral 2: bit 4 locks sprite number to port 0x303B
      next_.write(reg, value);
      sprites_.setLockstep((value & 0x10) != 0);
      return;
    case 0x19: // sprite clip window, four writes: X1, X2, Y1, Y2
      next_.write(reg, value);
      sprites_.writeClip(value);
      return;
    case 0x1C: // reset the clip window write positions
      next_.write(reg, value);
      if (value & 0x08) tilemapClipIndex_ = 0;
      if (value & 0x02) sprites_.resetClipIndex();
      if (value & 0x01) layer2ClipIndex_ = 0;
      return;
    case 0x34: // sprite number
      next_.write(reg, value);
      sprites_.setNumber(value);
      if (sprites_.lockstep()) sprites_.selectPort(value);
      return;
    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: // sprite attributes
      next_.write(reg, value);
      sprites_.writeAttribute(reg - 0x35, value, false);
      return;
    case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: // with auto-increment
      next_.write(reg, value);
      sprites_.writeAttribute(reg - 0x75, value, true);
      return;
    case 0x18: // Layer 2 clip window, four writes: X1, X2, Y1, Y2
      next_.write(reg, value);
      layer2Clip_[layer2ClipIndex_ & 0x03] = value;
      ++layer2ClipIndex_;
      return;
    case 0x1B: // tilemap clip window, four writes: X1, X2, Y1, Y2
      next_.write(reg, value);
      tilemapClip_[tilemapClipIndex_ & 0x03] = value;
      ++tilemapClipIndex_;
      return;
    default:
      next_.write(reg, value);
      return;
  }
}

// The Next runs its CPU faster than the ULA: at 28 MHz the same frame holds
// eight times the instructions. The ULA clock is therefore advanced by only
// 1/N of the CPU T-states, which keeps the raster, contention and audio on
// real time while the CPU gets the extra throughput.
u32 Machine::turboMultiplier() const {
  if (!isNext()) return 1;
  static constexpr u32 Multipliers[4] = {1, 2, 4, 8};
  return Multipliers[next_.turbo()];
}

// ===========================================================================
// Contention and floating bus
// ===========================================================================

u8 Machine::contention(u16 addr, u32 t) {
  if (!timings_.contended) return 0;

  // Every machine with a ULA contends on bank 5 at 0x4000. The 128K also
  // contends on 0xC000 when an odd bank is paged there; the +2A/+3 does not,
  // and the Pentagon has no contention at all (`contended` is false there).
  bool contended = false;
  if (addr >= 0x4000 && addr < 0x8000) {
    contended = true;
  } else if (addr >= 0xC000 && !isPlus3() && (page_[7].page & 2) != 0) {
    contended = true;
  }
  if (!contended) return 0;

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
  cpuTstates_ += cycles;

  // See turboMultiplier(): the ULA sees only a fraction of the CPU cycles.
  u32 remaining = cycles;
  if (const u32 mult = turboMultiplier(); mult != 1) {
    turboAccum_ += cycles;
    remaining = turboAccum_ / mult;
    turboAccum_ %= mult;
  }
  while (remaining > 0) {
    const u32 frameT = u32(tstates_ % tstatesPerFrame_);
    const u32 hc = frameT % tstatesPerLine_;
    const u32 step = std::min(remaining, tstatesPerLine_ - hc);

    tstates_ += step;
    remaining -= step;
    tape_.advance(step);
    if (hasAy()) ay_.tick(step);

    audioAccum_ += u64(step) * u64(sampleRate_);
    while (audioAccum_ >= u64(timings_.cpuHz)) {
      audioAccum_ -= timings_.cpuHz;

      float left = beeper_.sample();
      float right = left;
      if (hasAy()) {
        left += ay_.sampleLeft() * 0.5f;
        right += ay_.sampleRight() * 0.5f;
      }
      // The speaker is AC coupled, so remove the standing level; without this
      // a silent AY would push the output hard against the rails.
      const float outL = left - dcInL_ + 0.998f * dcOutL_;
      const float outR = right - dcInR_ + 0.998f * dcOutR_;
      dcInL_ = left;
      dcOutL_ = outL;
      dcInR_ = right;
      dcOutR_ = outR;

      audio_.push_back(std::clamp(outL, -1.0f, 1.0f));
      audio_.push_back(std::clamp(outR, -1.0f, 1.0f));
    }
    // Two seconds of interleaved stereo.
    const size_t limit = size_t(sampleRate_) * 4;
    if (audio_.size() > limit) {
      audio_.erase(audio_.begin(), audio_.begin() + long(audio_.size() - limit));
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

namespace {
// Identifiers for the layer priority table in register 0x15.
constexpr int LayerSprites = 0;
constexpr int LayerLayer2 = 1;
constexpr int LayerUla = 2;
} // namespace

std::array<int, 3> Machine::layerOrder() const {
  // 0x15 bits 4:2 list the layers from top to bottom. The two blending modes
  // (110 and 111) mix Layer 2 into the ULA instead of ordering them, so they
  // are treated as S L U for now.
  static constexpr std::array<std::array<int, 3>, 6> Table = {{
      {{LayerSprites, LayerLayer2, LayerUla}}, // S L U
      {{LayerLayer2, LayerSprites, LayerUla}}, // L S U
      {{LayerSprites, LayerUla, LayerLayer2}}, // S U L
      {{LayerLayer2, LayerUla, LayerSprites}}, // L U S
      {{LayerUla, LayerSprites, LayerLayer2}}, // U S L
      {{LayerUla, LayerLayer2, LayerSprites}}, // U L S
  }};
  const int priority = (next_.read(0x15) >> 2) & 0x07;
  return Table[size_t(priority < 6 ? priority : 0)];
}

int Machine::layerDepth(const std::array<int, 3>& order, int layer) {
  for (int depth = 0; depth < 3; ++depth) {
    if (order[size_t(depth)] == layer) return depth;
  }
  return 3;
}

void Machine::renderFrame() {
  const std::array<int, 3> order = layerOrder();
  const int spriteDepth = layerDepth(order, LayerSprites);
  // 0x68 bit 7 blanks the ULA without touching the tilemap it shares a layer
  // with, so it has to be handled per pixel rather than by skipping the row.
  const bool ulaOn = !isNext() || (next_.read(0x68) & 0x80) == 0;
  const TilemapView tilemap = tilemapView();
  const Layer2View layer2 = layer2View();

  std::array<u32, ScreenWidth> ulaRow{};
  for (int r = 0; r < FrameHeight; ++r) {
    int line = 0;
    if (r < BorderHeight) {
      line = r;
    } else if (r >= BorderHeight + ScreenHeight) {
      line = timings_.topBorder + ScreenHeight + (r - BorderHeight - ScreenHeight);
    } else {
      line = timings_.topBorder + (r - BorderHeight);
    }
    if (line < 0 || line >= int(borderLines_.size())) line = 0;
    const u32 border = paperColour(borderLines_[size_t(line)] & 0x07);

    const int y = r - BorderHeight;
    const bool screenRow = y >= 0 && y < ScreenHeight;
    if (screenRow && ulaOn) renderUlaRow(y, ulaRow);

    u32* row = framebuffer_.data() + size_t(r) * FrameWidth;
    u8* occluders = occluders_.data() + size_t(r) * FrameWidth;

    for (int x = 0; x < FrameWidth; ++x) {
      const bool inWindow =
          screenRow && x >= BorderWidth && x < BorderWidth + ScreenWidth;
      const int ux = x - BorderWidth;

      const u32 ulaRgb = (ulaOn && inWindow) ? ulaRow[size_t(ux)] : Transparent;
      bool ulaOver = false;
      const u32 tileRgb = tilemapPixel(tilemap, x, r, ulaOver);

      // The tilemap is part of the ULA's display layer. The tile's own bit
      // says which of the two wins, unless the tilemap is forced on top or
      // the two are combined in stencil mode.
      u32 uRgb = ulaRgb;
      if (tileRgb != Transparent) {
        if (ulaRgb == Transparent) {
          uRgb = tileRgb;
        } else if (tilemap.stencil) {
          uRgb = ulaRgb & tileRgb;
        } else if (tilemap.tilemapOverUla) {
          uRgb = tileRgb;
        } else {
          uRgb = ulaOver ? ulaRgb : tileRgb;
        }
      }

      const u32 l2Rgb = inWindow ? layer2Pixel(layer2, ux, y) : Transparent;

      u32 rgb = border;
      u8 occluder = 0;
      for (int depth = 0; depth < 3; ++depth) {
        const int layer = order[size_t(depth)];
        if (layer == LayerSprites) continue; // sprites are a second pass
        const u32 colour = layer == LayerLayer2 ? l2Rgb : uRgb;
        if (colour == Transparent) continue;
        if (depth < spriteDepth) occluder = 1;
        rgb = colour;
        break;
      }
      row[x] = rgb;
      occluders[x] = occluder;
    }
  }

  renderSprites();
}


int Machine::ulaPalette() const {
  // 0x43 bit 1 picks the second ULA palette for rendering; bits 6:4 only
  // choose the palette the write registers land in.
  return (palette_.control() & 0x02) != 0 ? NextPalette::Ula1
                                          : NextPalette::Ula0;
}

u32 Machine::ulaColour(u8 index) const {
  if (!isNext()) return Palette[index & 0x0F];
  return palette_.colour(ulaPalette(), u8(index & 0x0F));
}

u32 Machine::paperColour(u8 index) const {
  if (!isNext()) return Palette[index & 0x0F];
  return palette_.colour(ulaPalette(), u8(16 + (index & 0x0F)));
}

Machine::Layer2View Machine::layer2View() const {
  Layer2View view;
  if (!isNext() || (next_.read(0x69) & 0x80) == 0) return view;

  const u8 control = next_.read(0x70);
  const int mode = (control >> 4) & 0x03;
  if (mode == 3) return view;

  view.active = true;
  view.fourBit = mode == 2;
  view.width = view.fourBit ? 640 : (mode == 1 ? 320 : 256);
  view.height = mode == 0 ? 192 : 256;
  view.palette = (palette_.control() & 0x04) != 0 ? NextPalette::Layer2Second
                                                  : NextPalette::Layer2First;
  view.transparency = next_.read(0x14);
  view.scrollX = next_.read(0x16) | ((next_.read(0x71) & 0x01) << 8);
  view.scrollY = next_.read(0x17);
  view.base = size_t(next_.read(0x12)) * size_t(BankSize);
  view.clipX1 = layer2Clip_[0];
  view.clipX2 = layer2Clip_[1];
  view.clipY1 = layer2Clip_[2];
  view.clipY2 = layer2Clip_[3];

  // The 320 and 640 wide modes are centred on the ULA's 256 pixel viewport.
  if (view.width != 256) view.scrollX += (view.width - ScreenWidth) / 2;
  if (view.height != 192) view.scrollY += (view.height - ScreenHeight) / 2;
  return view;
}

u32 Machine::layer2Pixel(const Layer2View& view, int x, int y) const {
  if (!view.active) return Transparent;

  const int stride = view.width * (view.fourBit ? 1 : 2) / 2;
  const int column = view.fourBit ? x * 2 : x;
  const int lx = (column + view.scrollX) % view.width;
  const int ly = (y + view.scrollY) % view.height;
  if (lx < view.clipX1 || lx > view.clipX2) return Transparent;
  if (ly < view.clipY1 || ly > view.clipY2) return Transparent;
  const size_t offset = view.base + size_t(ly) * size_t(stride) +
                        size_t(view.fourBit ? lx / 2 : lx);
  if (offset >= ram_.size()) return Transparent;

  const u8 packed = ram_[offset];
  const u8 pixel = view.fourBit ? u8((lx & 1) ? (packed & 0x0F) : (packed >> 4))
                                : packed;
  if (pixel == (view.fourBit ? u8(view.transparency & 0x0F) : view.transparency))
    return Transparent;

  const int entry = view.fourBit ? (((next_.read(0x70) & 0x0F) << 4) | pixel)
                                 : pixel;
  return palette_.colour(view.palette, u8(entry & 0xFF));
}

u32 Machine::tilemapPixel(const TilemapView& view, int x, int y,
                          bool& ulaOver) const {
  ulaOver = false;
  if (!view.active) return Transparent;

  // Register 0x1B clips in pairs of pixels across the 320 pixel surface.
  if (x < int(view.clip[0]) * 2 || x > int(view.clip[1]) * 2 + 1) return Transparent;
  if (y < int(view.clip[2]) || y > int(view.clip[3])) return Transparent;

  // 80 column tiles are half a pixel wide, so the 320 pixel surface keeps
  // every second one.
  const int nativeX = view.columns == 80 ? x * 2 : x;
  const int mapWidth = view.columns * 8;
  const int sx = (nativeX + view.scrollX) % mapWidth;
  const int sy = (y + view.scrollY) & 0xFF;

  const int column = sx >> 3;
  const int row = sy >> 3;
  const size_t entrySize = view.noAttributes ? 1 : 2;
  const size_t entry =
      (size_t(row) * size_t(view.columns) + size_t(column)) * entrySize;
  const size_t mapAddress = view.mapBank + ((view.mapOffset + entry) & view.mapMask);
  if (mapAddress >= ram_.size()) return Transparent;

  u16 tile = ram_[mapAddress];
  u8 attribute = view.defaultAttribute;
  if (!view.noAttributes) {
    const size_t attrAddress =
        view.mapBank + ((view.mapOffset + entry + 1) & view.mapMask);
    if (attrAddress >= ram_.size()) return Transparent;
    attribute = ram_[attrAddress];
  }
  // In 512 tile mode the "ULA on top" bit becomes the ninth tile number bit
  // and the ULA always covers the tilemap unless it is forced on top.
  if (view.fiveTwelve) {
    tile = u16(tile | ((attribute & 0x01) << 8));
    ulaOver = true;
  } else {
    ulaOver = (attribute & 0x01) != 0;
  }

  const int paletteOffset =
      view.textMode ? (int(attribute) >> 1) & 0x7F : (int(attribute) >> 4) & 0x0F;

  int cx = sx & 7;
  int cy = sy & 7;
  if (!view.textMode) {
    if (attribute & 0x08) cx = 7 - cx;
    if (attribute & 0x04) cy = 7 - cy;
    if (attribute & 0x02) {
      const int rotated = cx;
      cx = cy;
      cy = 7 - rotated;
    }
  }

  const size_t tileSize = view.textMode ? 8 : 32;
  const size_t defAddress =
      view.defBank + ((view.defOffset + size_t(tile) * tileSize) & view.defMask);
  const size_t defEnd = defAddress + (view.textMode ? size_t(cy) + 1
                                                    : size_t(cy) * 4 + 2);
  if (defEnd > ram_.size()) return Transparent;

  u8 pixel = 0;
  int index = 0;
  if (view.textMode) {
    // Text mode trades mirror and rotate for a seven bit palette offset: the
    // pair index picks the paper and ink colours.
    pixel = u8((ram_[defAddress + size_t(cy)] >> (7 - cx)) & 0x01);
    index = (paletteOffset << 1) | pixel;
  } else {
    const u8 packed = ram_[defAddress + size_t(cy) * 4 + size_t(cx >> 1)];
    pixel = (cx & 1) ? u8(packed & 0x0F) : u8(packed >> 4);
    index = (paletteOffset << 4) | pixel;
  }
  if (pixel == view.transparency) return Transparent;
  return palette_.colour(view.palette, u8(index & 0xFF));
}

Machine::TilemapView Machine::tilemapView() const {
  TilemapView view;
  if (!isNext()) return view;

  const u8 control = next_.read(0x6B);
  if ((control & 0x80) == 0) return view;

  view.active = true;
  view.textMode = (control & 0x08) != 0;
  view.noAttributes = (control & 0x20) != 0;
  view.fiveTwelve = (control & 0x02) != 0;
  view.tilemapOverUla = (control & 0x01) != 0;
  view.columns = (control & 0x40) ? 80 : 40;
  view.palette = (control & 0x10) ? NextPalette::TilemapSecond
                                  : NextPalette::TilemapFirst;
  view.scrollX = int(next_.read(0x30)) | ((next_.read(0x2F) & 0x03) << 8);
  view.scrollY = next_.read(0x31);
  view.transparency = u8(next_.read(0x4C) & 0x0F);
  view.defaultAttribute = next_.read(0x6C);
  view.stencil = (next_.read(0x68) & 0x01) != 0;

  // 0x6E and 0x6F hold the MSB of an address in bank 5 or, when bit 7 is set,
  // in the 8K bank 7, which wraps at its own end.
  const u8 map = next_.read(0x6E);
  view.mapBank = size_t((map & 0x80) ? 7 : 5) * BankSize;
  view.mapMask = (map & 0x80) ? 0x1FFF : 0x3FFF;
  view.mapOffset = size_t(map & 0x3F) * 256;
  const u8 def = next_.read(0x6F);
  view.defBank = size_t((def & 0x80) ? 7 : 5) * BankSize;
  view.defMask = (def & 0x80) ? 0x1FFF : 0x3FFF;
  view.defOffset = size_t(def & 0x3F) * 256;
  view.clip = tilemapClip_;
  return view;
}

void Machine::renderUlaRow(int y, std::array<u32, ScreenWidth>& row) const {
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
    const u32 inkRgb = ulaColour(flash ? paper : ink);
    const u32 paperRgb = paperColour(flash ? ink : paper);
    for (int b = 0; b < 8; ++b) {
      row[size_t(col * 8 + b)] = (pixels & (0x80u >> b)) ? inkRgb : paperRgb;
    }
  }
}



void Machine::renderSprites() {
  sprites_.clearStatus();
  if (!isNext()) return;
  const u8 control = next_.read(0x15);
  if ((control & 0x01) == 0) return; // sprites disabled

  const bool overBorder = (control & 0x02) != 0;
  const bool clipOverBorder = (control & 0x20) != 0;
  const bool zeroOnTop = (control & 0x40) != 0;
  const u8 transparency = next_.read(0x4B);
  const int palette = (palette_.control() & 0x08) != 0 ? NextPalette::SpritesSecond
                                                       : NextPalette::SpritesFirst;
  const auto& clip = sprites_.clip();
  const auto states = sprites_.resolve();
  std::fill(spriteDrawn_.begin(), spriteDrawn_.end(), 0);

  // The hardware prepares sprites in list order and can only manage about a
  // hundred unscaled ones per scanline. Whatever it never reached is left out
  // of the frame, and port 0x303B reports that the line ran over.
  constexpr int SpritesPerLine = 100;
  std::array<u8, FrameHeight> lineSprites{};
  std::array<bool, Sprites::Count> dropped{};
  for (int sprite = 0; sprite < Sprites::Count; ++sprite) {
    const SpriteState& state = states[size_t(sprite)];
    if (!state.visible) continue;
    const int height = Sprites::Size * state.scaleY;
    bool fits = true;
    for (int dy = 0; dy < height && fits; ++dy) {
      const int surfaceY = (state.y + dy) & 0x1FF;
      if (surfaceY >= FrameHeight) continue;
      fits = lineSprites[size_t(surfaceY)] < SpritesPerLine;
    }
    if (!fits) {
      dropped[size_t(sprite)] = true;
      sprites_.noteOverflow();
      continue;
    }
    for (int dy = 0; dy < height; ++dy) {
      const int surfaceY = (state.y + dy) & 0x1FF;
      if (surfaceY < FrameHeight) ++lineSprites[size_t(surfaceY)];
    }
  }

  for (int n = 0; n < Sprites::Count; ++n) {
    const int sprite = zeroOnTop ? Sprites::Count - 1 - n : n;
    if (dropped[size_t(sprite)]) continue;
    const SpriteState& state = states[size_t(sprite)];
    if (!state.visible) continue;

    const int scaleX = state.scaleX;
    const int scaleY = state.scaleY;
    const int width = Sprites::Size * scaleX;
    const int height = Sprites::Size * scaleY;
    const int originX = state.x;
    const int originY = state.y;
    const int offset = state.paletteOffset << 4;

    for (int dy = 0; dy < height; ++dy) {
      // Sprite coordinates wrap at 512, so a sprite hanging off the top or
      // the right edge reappears on the opposite side.
      const int surfaceY = (originY + dy) & 0x1FF;
      if (surfaceY >= FrameHeight) continue;
      const bool rowInScreen =
          surfaceY >= BorderHeight && surfaceY < BorderHeight + ScreenHeight;
      const size_t rowBase = size_t(surfaceY) * FrameWidth;
      u32* row = framebuffer_.data() + rowBase;

      for (int dx = 0; dx < width; ++dx) {
        const int surfaceX = (originX + dx) & 0x1FF;
        if (surfaceX >= FrameWidth) continue;

        // Sprites only reach into the border when over-border drawing is on.
        const bool inScreen = rowInScreen && surfaceX >= BorderWidth &&
                              surfaceX < BorderWidth + ScreenWidth;
        if (!inScreen && !overBorder) continue;

        // The compositor recorded the layers that sit above the sprites, so a
        // pixel of any of them hides the sprite.
        if (occluders_[rowBase + size_t(surfaceX)] != 0) continue;

        // The clip window is measured from the ULA origin unless the sprites
        // reach into the border, where it moves to the sprite origin and its
        // X coordinates count double.
        if (!overBorder || clipOverBorder) {
          int clipX = overBorder ? surfaceX : surfaceX - BorderWidth;
          if (overBorder) clipX >>= 1;
          const int clipY = overBorder ? surfaceY : surfaceY - BorderHeight;
          if (clipX < clip[0] || clipX > clip[1]) continue;
          if (clipY < clip[2] || clipY > clip[3]) continue;
        }

        const int index =
            sprites_.pixel(state, dx / scaleX, dy / scaleY, transparency);
        if (index < 0) continue;

        // Two sprites over the same pixel are what the collision flag is for.
        if (spriteDrawn_[rowBase + size_t(surfaceX)] != 0) {
          sprites_.noteCollision();
        } else {
          spriteDrawn_[rowBase + size_t(surfaceX)] = 1;
        }

        row[surfaceX] = palette_.colour(palette, u8((offset + index) & 0xFF));
      }
    }
  }
}

} // namespace mynext
