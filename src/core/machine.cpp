#include "core/machine.h"

#include "core/state.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#include "peripherals/cartridge.h"
#include "peripherals/kempston.h"
#include "peripherals/kempston_mouse.h"
#include "peripherals/printer.h"

#include "formats/dsk.h"
#include "formats/nex.h"
#include "formats/sna.h"
#include "formats/snapshot.h"
#include "formats/z80file.h"

namespace rainbow {

// ===========================================================================
// Machine descriptions
// ===========================================================================

const Timings& timingsFor(MachineType type) {
  // Values mirror the base_timings table in Fuse's libspectrum (see
  // docs/REFERENCES.md). Fields: cpu, ay, left, screen, right, hretrace,
  // top, vscreen, bottom, vretrace, intlen, contended.
  static const Timings t48{3500000, 0, 24, 128, 24, 48, 48, 192, 48, 24, 32, true};
  static const Timings t128{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 36, true};
  static const Timings tPlus3{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 32, true};
  static const Timings tPentagon{3584000, 1792000, 36, 128, 28, 32, 64, 192, 48, 16, 36, false};
  static const Timings tNext{3546900, 1773400, 24, 128, 24, 52, 48, 192, 48, 23, 36, true};

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

const char* machineId(MachineType type) {
  switch (type) {
    case MachineType::Spectrum48: return "48";
    case MachineType::Spectrum128: return "128";
    case MachineType::Plus2: return "plus2";
    case MachineType::Plus3: return "plus3";
    case MachineType::Pentagon: return "pentagon";
    case MachineType::Next: return "next";
  }
  return "48";
}

std::optional<MachineType> machineTypeFromId(const std::string& id) {
  for (MachineType type : {MachineType::Spectrum48, MachineType::Spectrum128, MachineType::Plus2,
                           MachineType::Plus3, MachineType::Pentagon, MachineType::Next}) {
    if (id == machineId(type)) return type;
  }
  // A few spellings people reach for that are not the ids themselves.
  if (id == "48k") return MachineType::Spectrum48;
  if (id == "128k") return MachineType::Spectrum128;
  if (id == "plus2a" || id == "+2a") return MachineType::Plus3;
  if (id == "+2") return MachineType::Plus2;
  if (id == "+3") return MachineType::Plus3;
  return std::nullopt;
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
  altRom_.assign(AltRomPages * PageSize, 0);
  ram_.assign(RamPages * PageSize, 0);
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);
  reset();
  installPeripherals();
  // The DMA reaches memory and IO through the machine, but is otherwise a
  // self-contained device.
  dma_.connect(
      [this](bool isIo, u16 address) {
        return isIo ? ioRead(address, cpuTstates_) : memRead(address, cpuTstates_);
      },
      [this](bool isIo, u16 address, u8 value) {
        if (isIo) {
          ioWrite(address, value, cpuTstates_);
        } else {
          memWrite(address, value, cpuTstates_);
        }
      });
}

void Machine::nextFirmwareBoot() {
  if (type_ != MachineType::Next) return;

  // The writes TBBLUE.FW 1.44 makes before it hands over, in the order it
  // makes them. Register 0x02 is left out on purpose: the firmware's only use
  // of it here is holding the ESP in reset, and there is no ESP to hold. The
  // keyjoy remapping is left out too, because the emulator reads the keyboard
  // directly instead of going through the PS/2 matrix the mapping describes.
  static constexpr u8 Writes[][2] = {
      {0x07, 0x03}, // 28 MHz
      {0x06, 0xA0}, // PS/2 keyboard takes priority over the matrix
      {0x03, 0x00}, // the boot ROM is done, leave configuration mode
      {0x05, 0xFB}, // joysticks come from the keyboard
      {0x11, 0x80}, // video mode 0, 50 Hz, scandoubler on
      {0x05, 0xFB},
      {0x09, 0x00}, // ROM bank select
      {0x04, 0x03},
      {0x05, 0x71}, // joy 1 and 2 enabled, no mouse buttons
      {0x06, 0x9D}, // PS/2 and matrix keyboard, both enabled
      {0x08, 0x1E}, // line interrupt off, 48K ULA
      {0x09, 0x00},
      {0x0A, 0x11}, // divMMC automap on, mouse DPI 1
      {0x82, 0xDA}, // Layer 2 RAM page and offset
      {0x83, 0x3F},
      {0x84, 0xFF},
      {0x85, 0x01},
      {0x03, 0xB3}, // machine type +3 with +3 timing
  };
  // The firmware programs the FPGA while it is itself in configuration mode,
  // so the gated registers above take the bits it means to set. The last
  // 0x03 write above carries machine type 3, which leaves the mode again --
  // exactly what the real firmware does before it jumps to NextZXOS.
  configMode_ = true;
  for (const auto& write : Writes) writeNextReg(write[0], write[1]);

  // The firmware's last act is a soft reset, which is what turns the
  // configuration above into a running NextZXOS: the reset keeps the
  // registers, drops into the boot mapping and starts the CPU at 0x0000.
  reset(true);
}

void Machine::reset(bool preserveRam) {
  cpu_.reset();
  cpu_.setExtendedInstructions(type_ == MachineType::Next);
  ula_.reset(type_ != MachineType::Spectrum48);
  keyboard_.reset();
  beeper_.reset();
  tape_.rewind();

  if (!preserveRam) {
    ram_.assign(RamPages * PageSize, 0);
    // The alt ROM is loaded by the boot ROM on every boot, so a cold start
    // leaves it blank rather than serving the previous session's image.
    altRom_.assign(AltRomPages * PageSize, 0);
    // A power cycle hands the machine back to the boot ROM, and the boot ROM
    // runs in configuration mode; a soft reset keeps whatever mode it is in.
    configMode_ = type_ == MachineType::Next;
  }
  framebuffer_.assign(FrameWidth * FrameHeight, Palette[0]);
  borderLines_.fill(0);

  // A hard reset (or a power cycle) is the one that reports itself as hard
  // through register 0x02, and the only one that clears the RAM.
  next_.reset(!preserveRam);
  palette_.reset();
  sprites_.reset();
  copper_.reset();
  dma_.reset();
  layer2Clip_ = {0, 255, 0, 191};
  layer2ClipIndex_ = 0;
  tilemapClip_ = {0, 159, 0, 255};
  tilemapClipIndex_ = 0;
  ulaClip_ = {0, 255, 0, 191};
  ulaClipIndex_ = 0;
  occluders_.assign(FrameWidth * FrameHeight, 0);
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
  sound_.reset();
  sound_.setChipSelectEnabled(isNext());
  sound_.setStereoMode(type_ == MachineType::Plus3 || isNext() ? Ay38912::Stereo::ABC
                                                              : Ay38912::Stereo::Mono);
  dacs_.reset();
  fdc_.reset();
  fdc_.setDisk(hasDiskController() && !disk_.empty() ? &disk_ : nullptr);
  if (preserveRam) {
    expansion_.softReset();
  } else {
    expansion_.reset();
  }
  // The card keeps its image across a reset; only the SPI handshake restarts.
  sdCard_.reset();
  spiSelect_ = 0xFF;
  // The divMMC latch always drops; only a power-on reset also clears its RAM,
  // because esxDOS leaves warm-boot markers there.
  if (preserveRam) {
    divmmc_.softReset();
  } else {
    divmmc_.hardReset();
  }

  tstates_ = 0;
  cpuTstates_ = 0;
  turboAccum_ = 0;
  frames_ = 0;
  ++resets_;
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
  if (!formats::loadDiskFile(path, image, error)) return false;
  disk_ = std::move(image);
  fdc_.reset();
  fdc_.setDisk(&disk_);
  return true;
}

// Serves the ROM's LD-BYTES routine from the loaded image instead of waiting
// for the real pulse train. On entry A holds the flag byte the caller expects,
// IX the destination and DE the length; on success the routine would leave
// with DE = 0 and carry set, which the exit at 0x05DF reproduces by loading A
// from a zero checksum and comparing it against 1.
//
// Custom loaders often copy LD-BYTES' own `INC D / EX AF,AF' / DEC D` prologue
// and jump straight to 0x0562, skipping the DI / OUT / PUSH HL part they have
// already done. The ROM picks the flag byte back up from AF' at 0x05A9, so that
// entry expects it in the shadow accumulator instead of A.
bool Machine::applyTapeFastLoad(u16 entry) {
  const formats::TapeBlock* block = tape_.seekDataBlock();
  if (block == nullptr || block->data.empty()) return false;

  Z80::Registers& regs = cpu_.regs();
  const u8 expected = entry == 0x0562 ? regs.a2 : regs.a;
  if (block->data[0] != expected) return false; // misaligned tape: let the ROM wait

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

bool Machine::loadDivMmcRom(const std::string& path, std::string& error) {
  if (!divmmc_.loadRomFile(path, error)) return false;
  // The ROM arriving late still has to answer a latch that is already set.
  updatePaging();
  // This is the last piece of a Next ROM set, so the firmware has everything
  // it needs to hand the machine over to NextZXOS. Every other machine just
  // got a divMMC ROM and boots from its own reset vector.
  if (type_ == MachineType::Next) nextFirmwareBoot();
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
  std::string ext;
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    ext = path.substr(dot);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  // .nex is not a snapshot of a running machine but a program image, so it gets
  // its own loader and reads the file itself.
  if (ext == ".nex") return loadNex(path, error);

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

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

bool Machine::loadNex(const std::string& path, std::string& error) {
  if (!isNext()) {
    error = "a .nex program needs the ZX Spectrum Next";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  formats::NexFile nex;
  if (!formats::loadNex(data, nex, error)) return false;
  applyNex(nex);
  return true;
}

// A .nex is a program plus the memory it wants rather than a snapshot of a
// running machine, so loading one starts from a reset Next and then replays what
// the file asks for. The order follows nexload: registers, loading screen, RAM,
// then the jump, so a bank that shares memory with a loading screen wins.
void Machine::applyNex(const formats::NexFile& nex) {
  reset();

  if (!nex.preserveNextRegs) {
    // The state nexload leaves behind, which a program written for a freshly
    // booted Next expects: Copper stopped, expansion bus off the way, AY and
    // Timex on, and every clip window covering the whole screen.
    writeNextReg(0x62, 0x00);
    writeNextReg(0x61, 0x00);
    // nexload reads these two back and edits them in place rather than writing
    // a constant, so a setting the caller made before loading survives. NR 0x06
    // gets DivMMC automap switched off and the multiface switched on; NR 0x08
    // gets locked paging and contention off, stereo back to ABC, SpecDrum,
    // Timex and TurboSound on, and leaves the internal-speaker bit alone.
    writeNextReg(0x06, u8((readNextReg(0x06) & ~0x10u) | 0x08u));
    writeNextReg(0x08, u8((readNextReg(0x08) & 0x10u) | 0xCEu));
    writeNextReg(0x07, 0x03);   // the loader leaves the machine at 28 MHz
    writeNextReg(0x12, 0x09);   // Layer 2 main and shadow banks
    writeNextReg(0x13, 0x0C);
    writeNextReg(0x14, 0xE3);   // Layer 2 global transparency
    writeNextReg(0x15, 0x01);   // Layer 2 priority, sprites visible
    writeNextReg(0x16, 0x00);
    writeNextReg(0x17, 0x00);
    writeNextReg(0x2D, 0x00);   // both DAC channels to silence
    writeNextReg(0x32, 0x00);   // LoRes scroll
    writeNextReg(0x33, 0x00);
    writeNextReg(0x4A, 0x00);   // transparent fallback colour
    writeNextReg(0x4B, 0xE3);   // sprite transparency index
    writeNextReg(0x1C, 0x0F);   // all four clip indices wrap immediately
    for (const u8 value : {u8(0), u8(255), u8(0), u8(191)}) writeNextReg(0x18, value);
    for (const u8 value : {u8(0), u8(255), u8(0), u8(191)}) writeNextReg(0x19, value);
    for (const u8 value : {u8(0), u8(255), u8(0), u8(191)}) writeNextReg(0x1A, value);
    for (const u8 value : {u8(0), u8(159), u8(0), u8(255)}) writeNextReg(0x1B, value);

    // The loader blanks the Layer 2 and sprite palettes and fills the ULA one
    // with its sixteen default colours repeated, so a program that only sets
    // the entries it uses does not inherit a leftover ramp. It fills the ULA
    // palette in two passes, one starting at index 0 and one at 128.
    static constexpr u8 DefaultUla[16] = {
        0x00, 0x02, 0xA0, 0xA2, 0x14, 0x16, 0xB4, 0xB6,
        0x00, 0x03, 0xE0, 0xE7, 0x1C, 0x1F, 0xFC, 0xFF};
    writeNextReg(0x43, 0x00);
    writeNextReg(0x42, 0x0F);   // ink mask off, flashing on
    for (const u8 start : {u8(0), u8(128)}) {
      writeNextReg(0x40, start);
      for (int entry = 0; entry < 128; ++entry) writeNextReg(0x41, DefaultUla[entry & 15]);
    }
    for (const u8 control : {u8(0x10), u8(0x20)}) {
      writeNextReg(0x43, control);
      writeNextReg(0x40, 0x00);
      for (int entry = 0; entry < 256; ++entry) writeNextReg(0x41, 0x00);
    }
  }

  // The palette block belongs to the loading screen. A LoRes screen takes its
  // colours from the LoRes Layer 2 palette and everything else from the first
  // Layer 2 palette, which is the distinction NR 0x43 bit 0 makes.
  if (nex.hasPalette) {
    writeNextReg(0x43, (nex.screenFlags & 0x04) != 0 ? 0x01 : 0x10);
    writeNextReg(0x40, 0x00);
    for (int entry = 0; entry < 256; ++entry) {
      palette_.setColour9(NextPalette::Layer2First, u8(entry), nex.palette[size_t(entry)]);
    }
  }

  const auto place = [this](size_t bank, const std::vector<u8>& bytes) {
    const size_t offset = bank * size_t(BankSize);
    const size_t count = std::min(bytes.size(), ram_.size() - offset);
    std::copy_n(bytes.begin(), count, ram_.data() + offset);
  };
  // The Layer 2 screen spans banks 9, 10 and 11 and the ULA screen is the top
  // 6912 bytes of bank 5, which is where 0x4000 lands on a Next.
  place(9, nex.layer2Screen);
  place(5, nex.ulaScreen);
  // A LoRes, Timex HiRes or HiCol loading screen covers 0x4000-0x6FFF. The
  // loader fills that in two reads, 0x4000-0x57FF and 0x6000-0x77FF, but the
  // two are contiguous in bank 5, so the 12K block lands as one run.
  place(5, nex.loresScreen);
  place(5, nex.hiresScreen);
  place(5, nex.hicolScreen);
  for (const formats::NexBank& bank : nex.banks) place(size_t(bank.number), bank.data);

  next_.setMmuSlot(6, u8(nex.entryBank * 2));
  next_.setMmuSlot(7, u8(nex.entryBank * 2 + 1));
  updatePaging();

  Z80::Registers& regs = cpu_.regs();
  regs.pc = nex.pc;
  regs.sp = nex.sp;
  // nexload runs the program with interrupts off; it enables them once it has
  // installed its own handler.
  regs.iff1 = false;
  regs.iff2 = false;

  ula_.write(0xFE, nex.border);
  beeper_.setLevel(ula_.beeper());
}

// ===========================================================================
// Execution
// ===========================================================================

void Machine::stepInstruction() {
  if (irqLine_) cpu_.interrupt();
  const u16 pc = cpu_.regs().pc;
  if ((pc == 0x0556 || pc == 0x0562) && tape_.fastLoad() && applyTapeFastLoad(pc)) return;
  if (pc == 0x0008 && esxDos_.trap(*this, cpu_.regs())) return;
  // Sampled after the interrupt, so the profile blames the handler rather than
  // the instruction the interrupt pushed off.
  if (profiling_) {
    ++samples_[pc];
    ++sampleTotal_;
  }
  ++instructions_;
  cpu_.step();
  // The divMMC's exit stubs at 0x1FF8 unmap the block once the instruction
  // that fetched them has finished.
  if (isNext()) divmmc_.afterInstruction();
}

std::optional<u16> Machine::runFrame() {
  const u64 target = frames_ + 1;
  // The guest can ask for a reset from inside an instruction (NextReg 0x02),
  // which rewinds the frame counter and would leave this loop waiting for a
  // frame that has already gone by. One reset is enough of a frame boundary.
  const u64 resets = resets_;
  while (frames_ < target && resets_ == resets) {
    // Only pay for the search once something is armed.
    if (!breakpoints_.empty()) {
      const u16 pc = cpu_.regs().pc;
      if (std::binary_search(breakpoints_.begin(), breakpoints_.end(), pc)) return pc;
    }
    stepInstruction();
  }
  return std::nullopt;
}

void Machine::setBreakpoint(u16 address) {
  const auto at = std::lower_bound(breakpoints_.begin(), breakpoints_.end(), address);
  if (at == breakpoints_.end() || *at != address) breakpoints_.insert(at, address);
}

void Machine::clearBreakpoint(u16 address) {
  const auto at = std::lower_bound(breakpoints_.begin(), breakpoints_.end(), address);
  if (at != breakpoints_.end() && *at == address) breakpoints_.erase(at);
}

bool Machine::hasBreakpoint(u16 address) const {
  return std::binary_search(breakpoints_.begin(), breakpoints_.end(), address);
}

void Machine::clearBreakpoints() { breakpoints_.clear(); }

void Machine::setProfiling(bool on) {
  if (on == profiling_) return;
  profiling_ = on;
  if (on) resetProfile();
}

void Machine::resetProfile() {
  samples_.fill(0);
  sampleTotal_ = 0;
}

void Machine::runCycles(u32 cycles) {
  const u64 target = cpuTstates_ + cycles;
  while (cpuTstates_ < target) stepInstruction();
}

// ===========================================================================
// Save states
// ===========================================================================

namespace {

// A state file starts with this word, then a version, the machine type and the
// ROM fingerprint, and ends with a checksum over all of it.
constexpr u8 kStateMagic[8] = {'R', 'B', 'N', 'S', 'T', 'A', 'T', 'E'};
// Bump whenever the field order or the set of fields changes. An older build
// refuses a newer state instead of reading it as garbage.
constexpr u32 kStateVersion = 2;
constexpr size_t kStateHeader = sizeof(kStateMagic) + 4 + 1 + 1 + 8;

} // namespace

// The single body both directions use. Anything derived -- the resolved page
// table, the audio buffer, the sprite collision scratch -- is left out and
// rebuilt by loadState, so a state stays small and cannot disagree with itself.
template <class Ar> void Machine::serialize(Ar& ar) {
  ar(cpu_);
  ar(ula_);
  ar(keyboard_);
  ar(beeper_);
  ar(sound_);
  ar(dacs_);
  ar(tape_);
  ar(fdc_);
  ar(next_);
  ar(palette_);
  ar(sprites_);
  ar(copper_);
  ar(dma_);

  ar(ram_);
  ar(framebuffer_);
  ar(borderLines_);

  ar(ramBank_);
  ar(screenBank_);
  ar(romBank_);
  ar(port7ffd_);
  ar(port1ffd_);
  ar(pagingLocked_);
  ar(configMode_);
  ar(specialPaging_);
  ar(romLoaded_);
  ar(layer2Clip_);
  ar(layer2ClipIndex_);
  ar(tilemapClip_);
  ar(tilemapClipIndex_);
  ar(ulaClip_);
  ar(ulaClipIndex_);

  ar(tstates_);
  ar(cpuTstates_);
  ar(turboAccum_);
  ar(frames_);
  ar(irqLine_);
  ar(irqEnd_);
}

void Machine::saveState(std::vector<u8>& out) {
  StateWriter writer;
  u32 version = kStateVersion;
  u8 type = u8(type_);
  u8 romLoaded = romLoaded_ ? 1 : 0;
  // The ROM is a file the user supplies, so it is fingerprinted rather than
  // stored: a state restored against a different ROM would run the wrong code.
  u64 romHash = stateChecksum(rom_.data(), rom_.size());
  writer(version);
  writer(type);
  writer(romLoaded);
  writer(romHash);
  serialize(writer);

  out.clear();
  out.insert(out.end(), std::begin(kStateMagic), std::end(kStateMagic));
  const std::vector<u8>& payload = writer.bytes();
  out.insert(out.end(), payload.begin(), payload.end());
  const u64 sum = stateChecksum(out.data(), out.size());
  for (int i = 0; i < 8; ++i) out.push_back(u8(sum >> (8 * i)));
}

// Both ends of the stream, so the body above is instantiated for each.
template void Machine::serialize<StateWriter>(StateWriter&);
template void Machine::serialize<StateReader>(StateReader&);

bool Machine::peekStateType(const std::vector<u8>& in, MachineType& type) {
  if (in.size() < kStateHeader) return false;
  if (!std::equal(std::begin(kStateMagic), std::end(kStateMagic), in.begin())) return false;
  u32 version = 0;
  for (int i = 0; i < 4; ++i) version |= u32(in[8 + size_t(i)]) << (8 * i);
  if (version != kStateVersion) return false;
  const u8 raw = in[12];
  if (raw > u8(MachineType::Next)) return false;
  type = MachineType(raw);
  return true;
}

bool Machine::loadState(const std::vector<u8>& in, std::string& error) {
  if (in.size() < kStateHeader + 8) {
    error = "not a save state";
    return false;
  }
  if (!std::equal(std::begin(kStateMagic), std::end(kStateMagic), in.begin())) {
    error = "not a save state";
    return false;
  }
  u64 stored = 0;
  for (int i = 0; i < 8; ++i) stored |= u64(in[in.size() - size_t(8 - i)]) << (8 * i);
  if (stored != stateChecksum(in.data(), in.size() - 8)) {
    error = "the state is damaged (checksum mismatch)";
    return false;
  }

  // The header is read on its own, before anything in the machine is touched.
  StateReader header(in.data() + sizeof(kStateMagic), kStateHeader - sizeof(kStateMagic));
  u32 version = 0;
  header(version);
  if (version != kStateVersion) {
    error = "the state was written by another version (" + std::to_string(version) +
            ", this build writes " + std::to_string(kStateVersion) + ")";
    return false;
  }
  u8 type = 0;
  header(type);
  if (type != u8(type_)) {
    error = std::string("the state belongs to a ") + machineName(MachineType(type)) +
            ", not a " + machineName(type_);
    return false;
  }
  u8 romLoaded = 0;
  u64 romHash = 0;
  header(romLoaded);
  header(romHash);
  if (romLoaded != (romLoaded_ ? 1 : 0) ||
      romHash != stateChecksum(rom_.data(), rom_.size())) {
    error = "the state was saved with a different ROM";
    return false;
  }

  // The body is parsed twice: once into a scratch machine, to prove the whole
  // of it reads, and only then for real. A file that stops halfway therefore
  // leaves this machine untouched instead of half-rewritten.
  const u8* body = in.data() + kStateHeader;
  const size_t bodySize = in.size() - kStateHeader - 8;
  {
    Machine scratch(type_);
    StateReader probe(body, bodySize);
    scratch.serialize(probe);
    if (!probe.ok()) {
      error = probe.error();
      return false;
    }
    // Catches a field added without bumping the version, which the checksum
    // cannot see because the writer produced a consistent file.
    if (!probe.consumedAll()) {
      error = "the state has more data than this build understands";
      return false;
    }
  }

  StateReader reader(body, bodySize);
  serialize(reader);
  // The page table is derived from the paging registers the state just
  // restored, so it is rebuilt rather than trusted.
  updatePaging();
  fdc_.setDisk(hasDiskController() && !disk_.empty() ? &disk_ : nullptr);
  audio_.clear();
  audioAccum_ = 0;
  return true;
}

// ===========================================================================
// Memory
// ===========================================================================

// The +2A/+3 "special" paging mode replaces the whole 64K with RAM banks 0-3,
// which is what CP/M runs in. Every other machine keeps bank 5 at 0x4000 and
// bank 2 at 0x8000, with 0xC000 switchable.
u8 Machine::memRead(u16 addr, u32 t) {
  if (expansion_.memoryHooked()) {
    u8 value = 0xFF;
    if (expansion_.memRead(addr, t, false, value)) return value;
  }
  if (isNext()) {
    u8 value = 0xFF;
    if (divmmc_.read(addr, false, cpu_.regs().a, value)) return value;
  }
  return memReadBase(addr, t);
}

// The M1 cycle is where an automapping device earns its keep, so it is offered
// to the bus with the flag set before the machine decodes the address.
u8 Machine::memFetch(u16 addr, u32 t) {
  if (expansion_.memoryHooked()) {
    u8 value = 0xFF;
    if (expansion_.memRead(addr, t, true, value)) return value;
  }
  if (isNext()) {
    u8 value = 0xFF;
    if (divmmc_.read(addr, true, cpu_.regs().a, value)) return value;
  }
  return memReadBase(addr, t);
}

void Machine::memWrite(u16 addr, u8 value, u32 t) {
  if (expansion_.memoryHooked() && expansion_.memWrite(addr, value, t)) return;
  if (isNext() && divmmc_.write(addr, value)) return;
  memWriteBase(addr, value, t);
}

u8 Machine::memReadBase(u16 addr, u32) {
  // A cartridge takes over the bottom 16K entirely, exactly as an Interface 2
  // does: the machine's own ROM is not reachable while it is plugged in.
  if (cartRom_ != nullptr && addr < 0x4000) {
    return (*cartRom_)[addr & (cartRom_->size() - 1)];
  }
  const Page& page = page_[addr >> 13];
  u8 alt = 0;
  if (!page.ram && nextAltRomPage(addr, false, alt)) {
    return altRom_[size_t(alt) * PageSize + (addr & (PageSize - 1))];
  }
  const size_t offset = size_t(page.page) * PageSize + (addr & (PageSize - 1));
  return page.ram ? ram_[offset] : rom_[offset];
}

void Machine::memWriteBase(u16 addr, u8 value, u32) {
  const Page& page = page_[addr >> 13];
  if (!page.ram) {
    u8 alt = 0;
    if (nextAltRomPage(addr, true, alt)) {
      altRom_[size_t(alt) * PageSize + (addr & (PageSize - 1))] = value;
    }
    return;
  }
  ram_[size_t(page.page) * PageSize + (addr & (PageSize - 1))] = value;
}

// ===========================================================================
// I/O
// ===========================================================================

u8 Machine::ioRead(u16 port, u32 t) {
  u8 value = 0xFF;
  // The hook is the last stop on the way to the CPU, so a recording captures
  // what a program actually saw, whichever module answered the read.
  if (expansion_.ioRead(port, t, value)) {
    return ioReadHook_ != nullptr ? ioReadHook_->ioRead(port, value) : value;
  }
  value = ioReadBase(port, t);
  return ioReadHook_ != nullptr ? ioReadHook_->ioRead(port, value) : value;
}

void Machine::ioWrite(u16 port, u8 value, u32 t) {
  if (expansion_.ioWrite(port, value, t)) return;
  ioWriteBase(port, value, t);
}

u8 Machine::ioReadBase(u16 port, u32) {
  if (isNext()) {
    if (port == 0x243B) return next_.selected();
    if (port == 0x253B) return readNextReg(next_.selected());
    if (port == 0x303B) {
      // The sprite status flags clear themselves as they are read.
      const u8 status = sprites_.readStatus();
      sprites_.clearStatus();
      return status;
    }
    // 0x6B is the zxnDMA; 0x0B is the same device in Zilog compatibility mode,
    // where a block is one byte longer than the programmed length.
    if ((port & 0x00FF) == 0x6B) return dma_.read(false);
    if ((port & 0x00FF) == 0x0B) return dma_.read(true);
    // ULA+ data port; the register port 0xBF3B is write only.
    if ((port & 0xC0FF) == 0xC03B) {
      const u8 reg = ula_.ulaPlusRegister();
      if ((reg & 0xC0) == 0x00) return palette_.read8(ulaPalette(), u8(reg & 0x3F));
      return u8(ula_.ulaPlusMode() & 0x01);
    }
    // The divMMC control latch. The boot ROM writes it at 0x1EDB and then
    // checks that 0x0004 reads back "DV", so this must answer before the ROM is
    // even considered.
    {
      u8 value = 0xFF;
      if (divmmc_.ioRead(port, value)) return value;
    }
    // Reading the SPI data port clocks one byte out of the selected device, so
    // this is a read with a side effect rather than a plain register.
    if ((port & 0x00FF) == 0xEB) {
      u8 v = sdCard_.transfer(0xFF);
      return v;
    }
    if ((port & 0x00FF) == 0xE7) return spiSelect_;
  }

  if ((port & 0x0001) == 0) {
    if ((port & 0x00FF) == 0xFE) {
      u8 value = ula_.read(port, keyboard_);
      // Only a running tape drives the EAR input. With the motor stopped the
      // line idles low, which is the 0xBF the ULA already reports and what the
      // ROM loader and the Z80 conformance suites expect from an idle machine.
      if (tape_.playing()) value = u8((value & ~0x40) | (tape_.ear() ? 0x40 : 0x00));
      return value;
    }
    return floatingBus();
  }

  if (hasAy()) {
    if ((port & 0xC002) == 0x8000) return sound_.read(); // 0xBFFD data
    if ((port & 0xC002) == 0xC000) return 0xFF;       // 0xFFFD status
  }

  if (hasDiskController()) {
    if ((port & 0xF002) == 0x2000) return fdc_.readStatus(); // 0x2FFD
    if ((port & 0xF002) == 0x3000) return fdc_.readData();   // 0x3FFD
  }

  return 0xFF;
}

void Machine::ioWriteBase(u16 port, u8 value, u32) {
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
    if ((port & 0x00FF) == 0x6B) { // zxnDMA
      dma_.write(value, false);
      return;
    }
    if ((port & 0x00FF) == 0x0B) { // zxnDMA in Zilog compatibility mode
      dma_.write(value, true);
      return;
    }
    if ((port & 0xC0FF) == 0x803B) { // ULA+ register select
      ula_.writeUlaPlusRegister(value);
      return;
    }
    if ((port & 0x00FF) == DivMmc::PortControl) { // divMMC control latch
      divmmc_.ioWrite(port, value);
      return;
    }
    if ((port & 0x00FF) == 0xE7) { // SPI chip select, active low per device
      spiSelect_ = value;
      // One device is selected by pulling exactly one of bits 3:0 low, so
      // 0xFE reaches card 0 and 0xFD card 1. Anything else -- no bit low, two
      // bits low, or one of the Pi's own SPI sockets -- releases the card.
      const u8 selected = u8(~value & 0x0F);
      sdCard_.select(selected == 0x01 || selected == 0x02);
      return;
    }
    if ((port & 0x00FF) == 0xEB) { // SPI data: clock one byte into the card
      sdCard_.transfer(value);
      return;
    }
    if ((port & 0xC0FF) == 0xC03B) { // ULA+ data
      const u8 reg = ula_.ulaPlusRegister();
      if ((reg & 0xC0) == 0x00) {
        palette_.write8(ulaPalette(), u8(reg & 0x3F), value);
      } else if ((reg & 0xC0) == 0x40) {
        ula_.setUlaPlusMode(value);
      }
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
      sound_.select(value);
      return;
    }
    if ((port & 0xC002) == 0x8000) { // 0xBFFD
      sound_.write(value);
      return;
    }
  }

  // Timex SCLD register. The port is decoded on the low byte alone, so 0xBFFF
  // and 0xFFFF reach it too, but the AY has already claimed 0xFFFF above. On
  // the Next the address only carries the Timex register while Timex mode is
  // enabled in NR 0x82 bit 0; otherwise the firmware's palette traffic on the
  // same port must not be read as a display-mode change, so it is dropped.
  if (isNext() && (port & 0x00FF) == 0xFF) {
    if ((next_.read(0x82) & 0x01) != 0) ula_.writeTimex(value);
    return;
  }

  if (isNext() && SpecDrum::handles(port)) {
    dacs_.write(port, value);
    return;
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

void Machine::installPeripherals() {
  // The callbacks stop at the machine's own decode, so a module that claims a
  // port cannot re-enter the bus by reading another one.
  HostServices host;
  host.host = this;
  host.ioRead = [](void* self, u16 port, u32 t) {
    return static_cast<Machine*>(self)->ioReadBase(port, t);
  };
  host.ioWrite = [](void* self, u16 port, u8 value, u32 t) {
    static_cast<Machine*>(self)->ioWriteBase(port, value, t);
  };
  host.memRead = [](void* self, u16 addr, u32 t) {
    return static_cast<Machine*>(self)->memReadBase(addr, t);
  };
  host.memWrite = [](void* self, u16 addr, u8 value, u32 t) {
    static_cast<Machine*>(self)->memWriteBase(addr, value, t);
  };
  host.nextRegRead = [](void* self, u8 reg) {
    return static_cast<Machine*>(self)->readNextReg(reg);
  };
  host.nextRegWrite = [](void* self, u8 reg, u8 value) {
    static_cast<Machine*>(self)->writeNextReg(reg, value);
  };
  host.joystick = [](void* self) { return static_cast<Machine*>(self)->kempston_; };

  expansion_.setHost(host);
  expansion_.setObserver([this] { refreshCartridge(); });

  // The joystick interface is what most software expects to find; everything
  // else is off until the user plugs it in.
  expansion_.add(std::make_unique<KempstonJoystick>(), true);
  expansion_.add(std::make_unique<KempstonMouse>(), false);
  expansion_.add(std::make_unique<ZxPrinter>(), false);
  expansion_.add(std::make_unique<Cartridge>(), false);
  refreshCartridge();
  syncDivMmcAutomap();
}

// The automap is board state that lives in the Next register file: the master
// switch is 0x0A bit 4 and 0xB8-0xBB say which addresses are entry points.
// A classic machine has no such registers, so there the module is only useful
// with automap on.
void Machine::syncDivMmcAutomap() {
  divmmc_.setAutomap(!isNext() || (next_.read(0x0A) & 0x10) != 0);
  divmmc_.setEntryPoints(next_.read(0xB8), next_.read(0xB9), next_.read(0xBA),
                         next_.read(0xBB));
}

void Machine::refreshCartridge() {
  cartRom_ = expansion_.rom();
}

void Machine::writePort7ffd(u8 value) {
  if (pagingLocked_) return;
  port7ffd_ = value;
  // On the Next the classic ports are a shorthand for MMU writes. The map is
  // rebuilt before the lock is taken, because a write that sets bit 5 still
  // pages its bank in first and only then freezes the ports.
  if (isNext()) legacyRemap(true, false);
  pagingLocked_ = (value & 0x20) != 0;
  updatePaging();
}

// 0x1FFD exists only on the +2A/+3: bit 0 selects the special (CP/M) paging
// mode, bit 2 is the high ROM bit and bit 3 starts the disk motor. Bit 1 is
// ignored in normal mode and only matters inside CP/M mode, where it picks the
// memory map together with bit 2 -- so it is not a lock. Paging is locked
// exclusively by bit 5 of 0x7FFD, which freezes this port too.
void Machine::writePort1ffd(u8 value) {
  if (pagingLocked_) return;
  // Leaving CP/M mode has to restore the middle of the address space, so the
  // transition is recorded before the shadow is overwritten.
  const bool leaving = (port1ffd_ & 0x01) != 0 && (value & 0x01) == 0;
  port1ffd_ = value;
  if (isNext()) legacyRemap(true, leaving);
  updatePaging();
}

// The Next selects one of four 16K ROM banks from three sources: the machine
// type in register 0x03, the 0x7FFD/0x1FFD bits the classic machines use, and
// the lock in register 0x8C bits 4-5, which overrides both. This mirrors the
// ROM multiplexer in zxnext.vhd, and it matters far beyond cosmetics: the
// divMMC only automaps while the bank it shadows is the one paged in, and
// NextZXOS switches banks constantly.
u8 Machine::nextRomBank() const {
  const u8 type = u8(next_.read(0x03) & 0x07);
  const u8 lock = u8((next_.read(0x8C) >> 4) & 0x03);
  const u8 from7ffd = u8((port7ffd_ >> 4) & 0x01);
  // Types 0 and 1 are 48K modes: the low ROM is always selected.
  if (type == 0x00 || type == 0x01) return 0;
  // Types 2 and 4 are 128K modes: only one ROM bit, so the lock contributes a
  // single bit as well.
  if (type == 0x02 || type == 0x04) return lock ? u8((lock >> 1) & 0x01) : from7ffd;
  // Everything else is the +3 layout, with a second bit from 0x1FFD bit 2.
  return lock ? lock : u8((((port1ffd_ >> 2) & 0x01) << 1) | from7ffd);
}

// Which of the two 16K alt ROM banks register 0x8C selects. The lock bits 4-5
// name it directly when they are set; otherwise it follows the same ROM bit the
// main ROM does, so the alt ROM tracks the machine's ROM switch.
u8 Machine::nextAltRomBank() const {
  const u8 type = u8(next_.read(0x03) & 0x07);
  const u8 lock = u8((next_.read(0x8C) >> 4) & 0x03);
  // A 48K machine has no 0x7FFD ROM bit to follow, so only lock value 1 picks
  // the low bank and everything else the high one.
  if (type == 0x00 || type == 0x01) return (lock == 0x01) ? 0 : 1;
  return lock ? u8(lock >> 1) : u8((port7ffd_ >> 4) & 0x01);
}

// Register 0x8C puts the alt ROM behind the main ROM: bit 7 enables it and bit
// 6 says which side is writable. With bit 6 clear reads come from the alt ROM
// and writes fall through to the (read-only) main ROM; with it set the boot ROM
// can fill the alt banks by writing to the ROM window.
bool Machine::nextAltRomPage(u16 addr, bool write, u8& page) const {
  if (!isNext()) return false;
  const u8 reg = next_.read(0x8C);
  if ((reg & 0x80) == 0) return false;
  if (((reg & 0x40) != 0) != write) return false;
  page = u8(nextAltRomBank() * 2 + ((addr >> 13) & 0x01));
  return true;
}

// divMMC entry points are only live while the ROM the divMMC shadows is paged
// in, so that an ordinary RST 8 in a running program is not intercepted. On a
// +3 machine that is ROM 3, on a 128K machine ROM 1, and in 48K mode always.
bool Machine::nextDivMmcRom3() const {
  const u8 type = u8(next_.read(0x03) & 0x07);
  if (type == 0x00 || type == 0x01) return true;
  if (type == 0x02 || type == 0x04) return romBank_ == 1;
  return romBank_ == 3;
}

void Machine::updatePaging() {
  if (isNext()) {
    // The Next keeps its map in the MMU registers, and the classic paging
    // ports only write into those same registers. The ULA still takes its
    // screen bank from 0x7FFD bit 3, which is how a game swaps between the
    // shadow screen and the one Layer 2 uses.
    romBank_ = nextRomBank();
    divmmc_.setRom3Present(nextDivMmcRom3());
    updateNextMmu();
    ramBank_ = u8(port7ffd_ & 0x07);
    screenBank_ = (port7ffd_ & 0x08) ? 7 : 5;
    return;
  }

  u8 rom = u8((port7ffd_ & 0x10) ? 1 : 0);
  if (isPlus3()) rom = u8(rom | ((port1ffd_ & 0x04) ? 2 : 0));
  romBank_ = u8(rom & 0x03);
  // The divMMC's ROM3 condition: on the +2A/+3 it is the DOS ROM. Automapping
  // RST 8 and the esxDOS traps only happens while that one is paged in, which
  // is what keeps a cold boot's RST 0 free of it.
  divmmc_.setRom3Present(romBank_ == 3);

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
  // Bit 3 of 0x7FFD only picks the bank the ULA fetches from. The window at
  // 0x4000 is hard-wired to bank 5 in normal paging mode; the shadow screen in
  // bank 7 is only reachable by paging it into 0xC000. Programs keep their
  // variables at 0x7F00..0x7FFF in bank 5 and expect them to stay visible
  // after selecting the shadow screen.
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
  for (int slot = 0; slot < 8; ++slot) {
    const u8 value = next_.mmuSlot(slot);
    Page& page = page_[size_t(slot)];
    if (value == NextRegFile::RomPage) {
      page.ram = false;
      page.page = u16(romBank_ * 2 + (slot & 1));
    } else {
      page.ram = true;
      page.page = value;
    }
  }
}

// On the Next the three classic memory ports are a shorthand for MMU writes:
// 0x7FFD, 0x1FFD and register 0x8E rebuild the map from the 7FFD and 1FFD
// shadows instead of naming pages directly. Only the slots the classic hardware
// controls are rewritten, which is what lets a program park its own page in
// slots 2-5 and still switch the RAM bank with 0x7FFD. The exception is +3
// special paging, which owns all eight slots at once and has to hand the
// standard map back when it is left.
void Machine::legacyRemap(bool ramChange, bool leavingSpecial) {
  // 0x1FFD bit 0 is the +3 CP/M mode. Its map is fixed: one of four tables
  // picked by 0x1FFD bits 2:1, and every slot shows RAM, because the mode
  // exists so that CP/M can own the whole 64K.
  if ((port1ffd_ & 0x01) != 0) {
    static constexpr u8 Maps[4][8] = {
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
        {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
        {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x06, 0x07},
        {0x08, 0x09, 0x0E, 0x0F, 0x0C, 0x0D, 0x06, 0x07},
    };
    const u8* map = Maps[(port1ffd_ >> 1) & 0x03];
    for (int slot = 0; slot < 8; ++slot) next_.setMmuSlot(slot, map[slot]);
    return;
  }
  // Outside special paging 0x0000 and 0x2000 always show the selected ROM.
  next_.setMmuSlot(0, NextRegFile::RomPage);
  next_.setMmuSlot(1, NextRegFile::RomPage);
  // Leaving CP/M mode leaves the CP/M map behind in the middle of the address
  // space, so slots 2-5 go back to the standard 10/11/4/5.
  if (leavingSpecial) {
    next_.setMmuSlot(2, 0x0A);
    next_.setMmuSlot(3, 0x0B);
    next_.setMmuSlot(4, 0x04);
    next_.setMmuSlot(5, 0x05);
  }
  // The 0xC000 half only follows the RAM bank when this write actually changed
  // it, so a 0x1FFD write that only touches a ROM bit cannot steal the bank a
  // program paged in through the MMU slots.
  if (ramChange || leavingSpecial) {
    const u8 bank = u8(port7ffd_ & 0x07);
    next_.setMmuSlot(6, u8(bank * 2));
    next_.setMmuSlot(7, u8(bank * 2 + 1));
  }
}


// ===========================================================================
// Next register file
// ===========================================================================

u8 Machine::readNextReg(u8 reg) const {
  // The raster counters are the only registers whose value lives in the
  // machine rather than in the file itself.
  if (reg == 0x40) return palette_.index();
  // 0x41 and 0x44 read back the colour of the entry the index register points
  // at, not the last byte written. The Next boot ROM snapshots the palette with
  // them before it starts a program and restores it afterwards, so a stale
  // answer here wipes the palette.
  if (reg == 0x41) return palette_.read8(palette_.selected(), palette_.index());
  if (reg == 0x44) {
    const u16 colour = palette_.colour9(palette_.selected(), palette_.index());
    return u8((((colour >> 9) & 0x03) << 6) | (colour & 0x01));
  }
  if (reg == 0x61) return copper_.addressLsb();
  if (reg == 0x62) return copper_.control();
  if (reg == 0x2C) return dacs_.dac(1);
  if (reg == 0x2D) return dacs_.dac(0);
  if (reg == 0x2E) return dacs_.dac(2);
  if (reg == 0x1E || reg == 0x1F) {
    const u32 line = u32(tstates_ % tstatesPerFrame_) / tstatesPerLine_;
    return reg == 0x1E ? u8(line) : u8(line >> 8);
  }
  if (reg == 0x06) {
    // Bit 2 is the live configuration-mode flag, not the stored byte: the file
    // is reset with it clear while the machine can still be in the mode.
    return u8((next_.read(0x06) & 0xFB) | (configMode_ ? 0x04 : 0x00));
  }
  if (reg == 0x69) {
    // Composed from the classic ports, exactly like NR 0x8E: bit 6 is the Layer
    // 2 enable of port 0x123B, bit 3 the screen bank of port 0x7FFD and bits
    // 5:0 the Timex SCLD register. Bit 7 is the file's own Layer 2 enable.
    return u8((next_.read(0x69) & 0x80) | ((port123b_ & 0x02) << 6) |
              ((port7ffd_ & 0x08) << 3) | (ula_.timex() & 0x3F));
  }
  if (reg == 0x8E) {
    // Composed from the classic ports, so a guest that reads the register back
    // sees whatever it last wrote through either interface. In CP/M mode the
    // low ROM bit lives in 0x1FFD bit 1 instead of 0x7FFD bit 4.
    const u8 bank = isNext() ? u8(next_.mmuSlot(6) >> 1) : u8(port7ffd_ & 0x07);
    const bool special = (port1ffd_ & 0x01) != 0;
    const u8 rom = u8((isPlus3() || isNext()) ? ((port1ffd_ & 0x04) ? 2 : 0) : 0);
    const u8 lowRom = special ? u8((port1ffd_ & 0x02) ? 1 : 0) : u8((port7ffd_ & 0x10) ? 1 : 0);
    return u8((bank << 4) | 0x08 | (rom | lowRom) | (special ? 0x04 : 0x00));
  }
  return next_.read(reg);
}

void Machine::writeNextReg(u8 reg, u8 value) {
  switch (reg) {
    case 0x02: // bit 3 raises the multiface NMI, bit 2 the divMMC one, bit 1
               // asks for a hard reset and bit 0 for a soft one
      if ((value & 0x0C) != 0) cpu_.nmi();
      if ((value & 0x03) != 0) reset((value & 0x02) == 0);
      return;
    case 0x0A: { // peripheral 5: bit 4 is the divMMC automap master switch
      // Bits 7:5 are the machine configuration and only reachable while the
      // ROM is in configuration mode; the rest is always writable.
      const u8 previous = next_.read(reg);
      next_.write(reg, u8((value & 0x1F) | (configMode_ ? (value & 0xE0) : (previous & 0xE0))));
      divmmc_.setAutomap((value & 0x10) != 0);
      return;
    }
    case 0x06: { // peripheral 1: bit 2 reports configuration mode, it is not data
      const u8 previous = next_.read(reg);
      next_.write(reg, u8((value & 0xFB) | (configMode_ ? (value & 0x04) : (previous & 0x04))));
      return;
    }
    case 0xB8:   // the entry points the divMMC watches
    case 0xB9:   // ...which of them need the +3 DOS ROM paged in
    case 0xBA:   // ...and whether the map happens before or after the fetch
    case 0xBB: { // ...and the second entry-point group
      next_.write(reg, value);
      divmmc_.setEntryPoints(next_.read(0xB8), next_.read(0xB9), next_.read(0xBA),
                             next_.read(0xBB));
      return;
    }
    case 0x03: {
      // The display timing bits are locked unless bit 7 unlocks them.
      u8 type = next_.read(0x03);
      if ((value & 0x80) != 0) type = u8((type & 0x8F) | (value & 0x70));
      next_.write(0x03, type);
      // The low three bits are the configuration-mode selector rather than
      // stored data: 7 enters the mode and any other non-zero value leaves it.
      if ((value & 0x07) == 0x07) {
        configMode_ = true;
      } else if ((value & 0x07) != 0) {
        configMode_ = false;
      }
      // The machine type decides which ROM the classic paging ports select.
      updatePaging();
      return;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x8C: // alternate ROM
    case 0x8F: // mapping mode: only the standard 128K/+3 layout is modelled
      next_.write(reg, value);
      updatePaging();
      return;
    case 0x8E: { // the 128K memory map as a single register
      // Bits 0 and 1 are the two ROM select bits of ports 0x7FFD and 0x1FFD and
      // bit 2 the +2A/+3 paging mode, all of which are visible as the classic
      // ports. The RAM bank is only taken when bit 3 asks for it, which is how
      // a caller can change the ROM without disturbing the RAM mapping. The
      // 0x7FFD lock does not reach this register: it freezes the ports, not the
      // register interface, and NextZXOS' `RST 8` dispatcher relies on that to
      // select ROM 3 with a locked 0x7FFD.
      if ((value & 0x08) != 0) port7ffd_ = u8((port7ffd_ & 0xF8) | ((value >> 4) & 0x07));
      // The low ROM bit only drives 0x7FFD outside CP/M mode; inside it, that
      // bit moves to 0x1FFD bit 1, which is where this register reads it back.
      const bool special = (value & 0x04) != 0;
      if (!special) port7ffd_ = u8((port7ffd_ & ~0x10) | ((value & 0x01) ? 0x10 : 0x00));
      const bool leaving = (port1ffd_ & 0x01) != 0 && !special;
      port1ffd_ = u8((port1ffd_ & ~0x07) | (special ? 0x01 : 0x00) |
                     ((value & 0x02) ? 0x04 : 0x00) | ((value & 0x01) ? 0x02 : 0x00));
      if (isNext()) legacyRemap((value & 0x08) != 0, leaving);
      updatePaging();
      return;
    }
    case 0x10: // Layer 2 resolution: only reachable in configuration mode, and
               // bits 6:5 belong to the file rather than to this write
      if (configMode_) {
        next_.write(reg, u8((next_.read(reg) & 0x60) | (value & 0x1F)));
      }
      return;
    case 0x11: // Layer 2 scroll MSB, likewise configuration mode only
      if (configMode_) next_.write(reg, u8(value & 0x07));
      return;
    case 0x85: // internal port enable: bit 7 selects the write mode, bits 3:0
    case 0x89: // the enables themselves; the upper bits read back as written
      next_.write(reg, u8(value & 0x8F));
      return;
    case 0x40: // palette index
      next_.write(reg, value);
      palette_.setIndex(value);
      return;
    case 0x41: // 8-bit palette colour; the port is a write-only data path, so
               // nothing is stored and a read back answers from the palette
      palette_.write8(value);
      return;
    case 0x43: // palette control
      next_.write(reg, value);
      palette_.setControl(value);
      return;
    case 0x44: // 9-bit palette colour
      palette_.write9(value);
      return;
    case 0x60: // Copper program byte
      copper_.writeData8(value);
      return;
    case 0x61: // Copper write index, low byte
      copper_.writeAddressLsb(value);
      return;
    case 0x62: // Copper write index high bits and control mode
      copper_.writeControl(value);
      return;
    case 0x63: // Copper program word, committed after the second byte
      copper_.writeData16(value);
      return;
    case 0x08: // peripheral 3: TurboSound, stereo arrangement, DACs
      next_.write(reg, value);
      sound_.setEnabled((value & 0x02) != 0);
      sound_.setStereoMode((value & 0x20) ? Ay38912::Stereo::ACB : Ay38912::Stereo::ABC);
      return;
    case 0x09: // peripheral 2: AY mono bits, bit 4 locks the sprite number
      next_.write(reg, value);
      sprites_.setLockstep((value & 0x10) != 0);
      for (int i = 0; i < TurboSound::ChipCount; ++i) {
        sound_.setChipMono(i, (value & (0x20 << i)) != 0);
      }
      return;
    case 0x2C: // DAC B mirror, left
      next_.write(reg, value);
      dacs_.writeDac(1, value);
      return;
    case 0x2D: // the mono mirror: DACs A and D together
      next_.write(reg, value);
      dacs_.writeDac(0, value);
      dacs_.writeDac(3, value);
      return;
    case 0x2E: // DAC C mirror, right
      next_.write(reg, value);
      dacs_.writeDac(2, value);
      return;
    case 0x19: // sprite clip window, four writes: X1, X2, Y1, Y2
      next_.write(reg, value);
      sprites_.writeClip(value);
      return;
    case 0x1C: // reset the clip window write positions
      next_.write(reg, value);
      if (value & 0x08) tilemapClipIndex_ = 0;
      if (value & 0x04) ulaClipIndex_ = 0;
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
    case 0x1A: // ULA clip window, four writes: X1, X2, Y1, Y2
      next_.write(reg, value);
      ulaClip_[ulaClipIndex_ & 0x03] = value;
      ++ulaClipIndex_;
      return;
    case 0x69: // Layer 2 control is mostly a mirror of the classic ports, so
               // the write is copied into them as well: bit 6 is the Layer 2
               // enable of port 0x123B, bit 3 the screen bank of port 0x7FFD
               // and bits 5:0 the Timex SCLD register. Bit 7 keeps its own
               // storage in the file, which is what enables Layer 2 here.
      next_.write(reg, value);
      port123b_ = u8((port123b_ & ~0x02) | ((value >> 6) & 0x02));
      if (u8(value & 0x08) != (port7ffd_ & 0x08)) {
        port7ffd_ = u8((port7ffd_ & ~0x08) | (value & 0x08));
        updatePaging();
      }
      ula_.writeTimex(u8(value & 0x3F));
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

  // Every machine with a ULA contends on the memory it fetches: bank 5 at
  // 0x4000, and the RAM at 0xC000. The 128K and the grey +2 only contend there
  // while an odd bank is paged in, because the odd banks are the ones the gate
  // array can see; the +2A/+3 ASIC contends whatever bank it puts at 0xC000.
  // The Pentagon has no contention at all (`contended` is false there).
  bool contended = false;
  if (addr >= 0x4000 && addr < 0x8000) {
    contended = true;
  } else if (addr >= 0xC000) {
    contended = isPlus3() || (ramBank() & 1) != 0;
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

  // The ULA fetches two pixels per T-state, so each group of eight T-states
  // repeats the same bus pattern: six busy ones and two idle ones at the end.
  // The cycle is anchored to the first T-state of the display window, so every
  // machine opens its display on the first step. From there the step follows
  // the absolute T-state, which is what separates the machines: the 48K line is
  // 224 T-states, a multiple of eight, so it meets the same phase on every
  // line, while the 128K, the +3 and the Next use 228 and walk it by four.
  static constexpr u8 Pattern[8] = {6, 5, 4, 3, 2, 1, 0, 0};
  const u32 displayStart =
      tstatesPerLine_ * u32(timings_.topBorder) + timings_.leftBorder;
  return Pattern[(t - displayStart) & 7];
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

  // A DMA transfer with a prescalar hands the bus back to the CPU between
  // bytes, so it advances with the clock rather than inside one instruction.
  if (dma_.running() && dma_.timed()) {
    const u32 mult = turboMultiplier();
    dma_.advance(u64(cycles) * 8 / mult);
  }

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
    if (hasAy()) sound_.tick(step);
    expansion_.tick(step);

    audioAccum_ += u64(step) * u64(sampleRate_);
    while (audioAccum_ >= u64(timings_.cpuHz)) {
      audioAccum_ -= timings_.cpuHz;

      float left = beeper_.sample();
      float right = left;
      if (hasAy()) {
        // All three chips of the Next add up, exactly as they do on the board.
        left += sound_.sampleLeft() * 0.5f;
        right += sound_.sampleRight() * 0.5f;
      }
      if (isNext()) {
        left += dacs_.sampleLeft();
        right += dacs_.sampleRight();
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
    // Two seconds of interleaved stereo. Trim in chunks: once the buffer sits
    // at the limit the naive one-in-one-out erase memmoves ~700 KB per
    // scanline, which dwarfs the emulation itself.
    const size_t limit = size_t(sampleRate_) * 4;
    if (audio_.size() > limit + 4096) {
      audio_.erase(audio_.begin(), audio_.begin() + long(audio_.size() - limit));
    }

    const u32 newFrameT = u32(tstates_ % tstatesPerFrame_);
    if (newFrameT % tstatesPerLine_ == 0) {
      const u32 linesPerFrame = tstatesPerFrame_ / tstatesPerLine_;
      const u32 completed = (newFrameT == 0) ? (linesPerFrame - 1)
                                             : (newFrameT / tstatesPerLine_ - 1);
      if (completed < borderLines_.size()) borderLines_[completed] = ula_.border();

      if (newFrameT == 0) {
        if (videoEnabled_) renderFrame();
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

// The framebuffer holds 8-bit channels but the palette works in three bits per
// channel, so blending goes back to the palette's scale and clamps there. The
// rounding keeps a level 1 colour from collapsing onto black.
constexpr int channelOf(u32 argb, int shift) {
  return (int((argb >> shift) & 0xFF) * 7 + 128) / 255;
}

u32 averageColours(u32 first, u32 second) {
  const u32 r = (((first >> 16) & 0xFF) + ((second >> 16) & 0xFF) + 1) / 2;
  const u32 g = (((first >> 8) & 0xFF) + ((second >> 8) & 0xFF) + 1) / 2;
  const u32 b = ((first & 0xFF) + (second & 0xFF) + 1) / 2;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Priority modes 110 and 111 mix Layer 2 into the ULA rather than ordering the
// two: 110 adds the colours, 111 subtracts five from each channel. A layer that
// is transparent where the other is not is drawn on its own, unmixed.
u32 blendColours(u32 base, u32 layer, bool dark) {
  if (base == Machine::Transparent) return layer;
  if (layer == Machine::Transparent) return base;
  const int bias = dark ? 5 : 0;
  const auto channel = [&](int shift) {
    const int sum = channelOf(base, shift) + channelOf(layer, shift) - bias;
    return u16(sum < 0 ? 0 : (sum > 7 ? 7 : sum));
  };
  return NextPalette::toArgb(u16((channel(16) << 6) | (channel(8) << 3) | channel(0)));
}
} // namespace

std::array<int, 3> Machine::layerOrder() const {
  // 0x15 bits 4:2 list the layers from top to bottom. The two blending modes
  // (110 and 111) mix Layer 2 into the ULA instead of ordering them; sprites
  // stay on top and register 0x68 decides which half of the ULA is blended.
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

int Machine::rasterLineFor(int r) const {
  int line = 0;
  if (r < BorderHeight) {
    line = r;
  } else if (r >= BorderHeight + ScreenHeight) {
    line = timings_.topBorder + ScreenHeight + (r - BorderHeight - ScreenHeight);
  } else {
    line = timings_.topBorder + (r - BorderHeight);
  }
  if (line < 0 || line >= int(borderLines_.size())) line = 0;
  return line;
}

void Machine::renderFrame() {
  // Every layer register is sampled per scanline, so the Copper can rewrite
  // them while the frame is being drawn and the change shows up on the line it
  // was made for.
  if (copper_.mode() == 3) copper_.resetProgramCounter();
  sprites_.clearStatus();

  std::array<u32, ScreenWidth> ulaRow{};
  for (int r = 0; r < FrameHeight; ++r) {
    const int line = rasterLineFor(r);
    copper_.runToLine(line, [this](u8 reg, u8 value) { writeNextReg(reg, value); });

    const std::array<int, 3> order = layerOrder();
    const int spriteDepth = layerDepth(order, LayerSprites);
    // 0x15 bits 4:2 modes 110 and 111 blend Layer 2 with the ULA instead of
    // ordering them, and 0x68 bits 6:5 pick which half of the ULA is the blend
    // colour: 00 the ULA alone, 10 the ULA and tilemap together, 11 the tilemap
    // alone and 01 no blending at all.
    const int priority = isNext() ? (next_.read(0x15) >> 2) & 0x07 : 0;
    const bool blending = priority >= 6;
    const int blendMode = (next_.read(0x68) >> 5) & 0x03;
    // 0x68 bit 7 blanks the ULA without touching the tilemap it shares a layer
    // with, so it has to be handled per pixel rather than by skipping the row.
    const bool ulaOn = !isNext() || (next_.read(0x68) & 0x80) == 0;
    const TilemapView tilemap = tilemapView();
    const Layer2View layer2 = layer2View();
    const u32 border = ulaBorderColour(borderLines_[size_t(line)] & 0x07);

    const int y = r - BorderHeight;
    const bool screenRow = y >= 0 && y < ScreenHeight;
    if (screenRow && ulaOn) renderUlaRow(y, ulaRow);

    u32* row = framebuffer_.data() + size_t(r) * FrameWidth;
    u8* occluders = occluders_.data() + size_t(r) * FrameWidth;

    for (int x = 0; x < FrameWidth; ++x) {
      const bool inWindow =
          screenRow && x >= BorderWidth && x < BorderWidth + ScreenWidth;
      const int ux = x - BorderWidth;

      const u32 ulaRgb =
          (ulaOn && inWindow && !ulaClipped(ux, y)) ? ulaRow[size_t(ux)] : Transparent;
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

      bool l2Priority = false;
      const u32 l2Rgb = inWindow ? layer2Pixel(layer2, ux, y, l2Priority) : Transparent;
      u32 rgb = border;
      u8 occluder = 0;
      bool blended = false;
      if (blending && blendMode != 0x01) {
        u32 base = uRgb;
        u32 front = Transparent;
        if (blendMode == 0x00) {
          base = ulaRgb;   // the ULA is the blend colour, the tilemap the front
          front = tileRgb;
        } else if (blendMode == 0x03) {
          base = tileRgb;  // and the other way round
          front = ulaRgb;
        }
        // Whichever half of the ULA layer is not blended keeps its own place,
        // which in these modes is above the sprites.
        if (front != Transparent) {
          rgb = front;
          occluder = 1;
        } else {
          const u32 mixed = blendColours(base, l2Rgb, priority == 7);
          if (mixed != Transparent) rgb = mixed;
          // Bit 7 of the Layer 2 colour survives the blend and lifts the pixel
          // above the sprites, which are otherwise on top in these modes.
          if (l2Priority && l2Rgb != Transparent) occluder = 1;
        }
        blended = true;
      }

      if (!blended) {
        // A Layer 2 colour flagged as a priority colour in the palette is drawn
        // above every other layer, whatever order 0x15 asks for.
        if (l2Priority && l2Rgb != Transparent) {
          rgb = l2Rgb;
          occluder = 1;
        } else {
          for (int depth = 0; depth < 3; ++depth) {
            const int layer = order[size_t(depth)];
            if (layer == LayerSprites) continue; // sprites are a second pass
            const u32 colour = layer == LayerLayer2 ? l2Rgb : uRgb;
            if (colour == Transparent) continue;
            if (depth < spriteDepth) occluder = 1;
            rgb = colour;
            break;
          }
        }
      }
      row[x] = rgb;
      occluders[x] = occluder;
    }

    renderSpriteRow(r);
  }
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

u32 Machine::layer2Pixel(const Layer2View& view, int x, int y,
                         bool& priority) const {
  priority = false;
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
  priority = (entry & 0x80) != 0;
  return palette_.colour(view.palette, u8(entry & 0xFF));
}

bool Machine::ulaClipped(int x, int y) const {
  return x < ulaClip_[0] || x > ulaClip_[1] || y < ulaClip_[2] ||
         y > ulaClip_[3];
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
  if (loresEnabled()) {
    renderLoresRow(y, row);
    return;
  }

  // Timex modes 1 and 2 move the attributes, and mode 6 is a different display
  // entirely. The modes without a documented behaviour of their own (3, 4, 5,
  // 7) reuse the second file like mode 1 does, which is what the Timex hardware
  // does with the bits it does not decode.
  const int mode = isNext() ? ula_.screenMode() : 0;
  if (mode == 6) {
    renderUlaHiresRow(y, row);
    return;
  }

  const size_t bankBase = size_t(screenBank_) * BankSize;
  const size_t altOffset = (mode & 0x01) != 0 ? 0x2000 : 0;
  const size_t pixelOffset = ((size_t(y) & 0xC0) << 5) + ((size_t(y) & 0x07) << 8) +
                             ((size_t(y) & 0x38) << 2);
  const size_t pixelBase = bankBase + altOffset + pixelOffset;
  // Hi-colour takes one attribute per character cell from the second file at
  // the same interleaved offset as the pixels.
  const size_t attrBase = mode == 2
                              ? bankBase + 0x2000 + pixelOffset
                              : bankBase + altOffset + 0x1800 + size_t(y >> 3) * 32;
  const bool flashOn = ((frames_ >> 4) & 1) != 0;

  for (int col = 0; col < 32; ++col) {
    const u8 pixels = ram_[pixelBase + size_t(col)];
    const u8 attr = ram_[attrBase + size_t(col)];
    u32 inkRgb = 0;
    u32 paperRgb = 0;
    u16 ink9 = 0xFFFF;
    u16 paper9 = 0xFFFF;
    ulaCellColours(attr, flashOn, inkRgb, paperRgb, ink9, paper9);
    // NextReg 0x14 makes the matching colour see-through, which is how a game
    // shows Layer 2 under the ULA without blanking the ULA layer.
    const bool inkClear = ulaTransparent(ink9);
    const bool paperClear = ulaTransparent(paper9);
    for (int b = 0; b < 8; ++b) {
      const bool on = (pixels & (0x80u >> b)) != 0;
      row[size_t(col * 8 + b)] = on ? (inkClear ? Transparent : inkRgb)
                                    : (paperClear ? Transparent : paperRgb);
    }
  }
}

void Machine::renderUlaHiresRow(int y, std::array<u32, ScreenWidth>& row) const {
  const size_t bankBase = size_t(screenBank_) * BankSize;
  const size_t pixelBase = bankBase + ((size_t(y) & 0xC0) << 5) +
                           ((size_t(y) & 0x07) << 8) + ((size_t(y) & 0x38) << 2);
  // The second file supplies the odd hi-res columns, and the two colours come
  // from the SCLD register: ink is the complement of paper and both are bright.
  const u8 hiresColour = u8(ula_.hiresColour());
  const u32 ink = ulaColour(u8(8 + (hiresColour ^ 0x07)));
  const u32 paper = paperColour(u8(8 + hiresColour));

  for (int col = 0; col < 32; ++col) {
    const u16 pixels = u16((ram_[pixelBase + size_t(col)] << 8) |
                           ram_[pixelBase + 0x2000 + size_t(col)]);
    // The two bytes are sixteen hi-res pixels, and each output pixel covers a
    // pair of them, so one character cell stays eight pixels wide.
    for (int pair = 0; pair < 8; ++pair) {
      const u32 left = (pixels & (0x8000u >> (pair * 2))) ? ink : paper;
      const u32 right = (pixels & (0x4000u >> (pair * 2))) ? ink : paper;
      row[size_t(col * 8 + pair)] = averageColours(left, right);
    }
  }
}

bool Machine::loresEnabled() const {
  return isNext() && (next_.read(0x15) & 0x80) != 0;
}

bool Machine::radastanMode() const {
  return (next_.read(0x6A) & 0x20) != 0;
}

void Machine::renderLoresRow(int y, std::array<u32, ScreenWidth>& row) const {
  // LoRes is a 128x96 image stretched over the whole 256x192 viewport, so every
  // dot is two display pixels wide and two rows tall. NextReg 0x32/0x33 scroll
  // it in half-dot steps and 0x6A bit 5 turns it into Radastan, four bits per
  // dot. The image is linear, unlike the interleaved ULA screen file.
  const u8 control = next_.read(0x6A);
  const bool radastan = radastanMode();
  const int paletteOffset = control & 0x0F;
  const int scrollX = next_.read(0x32);
  const int scrollY = next_.read(0x33);
  const int palette = ulaPalette();
  // 0x6A bit 4 XORs the Timex display file select, so Radastan can swap the two
  // 6K halves of the bank without disturbing the SCLD register.
  const bool secondFile =
      ((ula_.screenMode() & 0x01) != 0) != ((control & 0x10) != 0);
  const size_t bankBase = size_t(screenBank_) * BankSize;
  const size_t base = bankBase + (radastan && secondFile ? 0x2000 : 0);

  // The vertical scroll wraps inside the 192 line viewport, and the odd shape of
  // the wrap above 192 is what the core does when the two are added together.
  int scrolled = y + scrollY;
  if (scrolled >= 192) scrolled = ((((scrolled >> 6) + 1) & 0x03) << 6) |
                                  (scrolled & 0x3F);
  const int line = scrolled >> 1;

  size_t rowBase;
  if (radastan) {
    // 96 rows of 64 bytes: two dots per byte, 128 dots per row.
    rowBase = size_t(line & 0x7F) << 6;
  } else {
    // 96 rows of 128 bytes, the second 48 of which live 8K further up so that
    // the image fills 0x4000-0x57FF and 0x6000-0x77FF. A scrolled line past the
    // bottom of the image walks on into the unused tail of the bank.
    size_t offset = size_t(line) << 7;
    if (scrolled >= 96)
      offset = ((((offset >> 11) + 1) & 0x07) << 11) | (offset & 0x07FF);
    rowBase = offset & 0x3FFF;
  }

  // Radastan takes its sixteen colours from one block of the palette selected by
  // 0x6A bits 3:0, and only two of those bits apply when ULA+ owns the top 64
  // entries. LoRes keeps eight bits per dot and only shifts the high nibble.
  const bool ulaPlus = ulaPlusEnabled() && (next_.read(0x43) & 0x01) == 0;
  const u8 highNibble = radastan
                            ? (ulaPlus ? u8(0xC0 | (paletteOffset & 0x03))
                                       : u8((paletteOffset & 0x0F) << 4))
                            : 0;
  for (int x = 0; x < ScreenWidth; ++x) {
    const int px = (x + scrollX) & 0xFF;
    u8 entry;
    if (radastan) {
      const u8 packed = ram_[base + rowBase + size_t(px >> 2)];
      const u8 dot = (px & 0x02) != 0 ? u8(packed & 0x0F) : u8(packed >> 4);
      entry = u8(highNibble | dot);
    } else {
      const u8 packed = ram_[base + rowBase + size_t(px >> 1)];
      const u8 high = u8(((packed >> 4) + paletteOffset) & 0x0F);
      entry = u8((high << 4) | (packed & 0x0F));
    }
    row[size_t(x)] = palette_.colour(palette, entry);
  }
}

void Machine::ulaCellColours(u8 attr, bool flashOn, u32& ink, u32& paper,
                             u16& ink9, u16& paper9) const {
  ink9 = 0xFFFF;
  paper9 = 0xFFFF;
  if (!isNext()) {
    const u8 bright = u8(attr & 0x40) ? 8 : 0;
    const u8 inkIndex = u8((attr & 0x07) | bright);
    const u8 paperIndex = u8(((attr >> 3) & 0x07) | bright);
    const bool flash = (attr & 0x80) != 0 && flashOn;
    ink = ulaColour(flash ? paperIndex : inkIndex);
    paper = paperColour(flash ? inkIndex : paperIndex);
    return;
  }

  const int palette = ulaPalette();
  if ((palette_.control() & 0x01) != 0) {
    // ULANext turns the attribute into a pair of palette indices: the low bits
    // hold ink, the high bits paper, offset by 128.
    const u8 mask = next_.read(0x42);
    if (mask == 0xFF) {
      // Full ink: the paper and border colours come from register 0x4A.
      const u8 fallback = u8(next_.read(0x4A) & 0x0F);
      ink = palette_.colour(palette, attr);
      paper = palette_.colour(palette, u8(16 + fallback));
      ink9 = palette_.colour9(palette, attr);
      paper9 = palette_.colour9(palette, u8(16 + fallback));
    } else {
      const u8 inkIndex = u8(attr & mask);
      const u8 paperIndex = u8(128 + (attr & u8(~mask)));
      ink = palette_.colour(palette, inkIndex);
      paper = palette_.colour(palette, paperIndex);
      ink9 = palette_.colour9(palette, inkIndex);
      paper9 = palette_.colour9(palette, paperIndex);
    }
    return;
  }

  if (ulaPlusEnabled()) {
    // ULA+ keeps its 64 colours in the first entries of the ULA palette: ink in
    // the low half of each block of 32 and paper in the high half, with the
    // block chosen by FLASH and BRIGHT.
    const u8 block = u8(((attr & 0x80) ? 2 : 0) | ((attr & 0x40) ? 1 : 0));
    const u8 base = u8(block * 16);
    const u8 inkIndex = u8(base + (attr & 0x07));
    const u8 paperIndex = u8(base + 8 + ((attr >> 3) & 0x07));
    ink = palette_.colour(palette, inkIndex);
    paper = palette_.colour(palette, paperIndex);
    ink9 = palette_.colour9(palette, inkIndex);
    paper9 = palette_.colour9(palette, paperIndex);
    return;
  }

  const u8 bright = u8(attr & 0x40) ? 8 : 0;
  const u8 inkIndex = u8((attr & 0x07) | bright);
  const u8 paperIndex = u8(((attr >> 3) & 0x07) | bright);
  const bool flash = (attr & 0x80) != 0 && flashOn;
  const u8 shownInk = flash ? paperIndex : inkIndex;
  const u8 shownPaper = flash ? inkIndex : paperIndex;
  ink = ulaColour(shownInk);
  paper = paperColour(shownPaper);
  // Ink lives in the low half of the ULA palette and paper in the high half.
  ink9 = palette_.colour9(palette, u8(shownInk & 0x0F));
  paper9 = palette_.colour9(palette, u8(16 + (shownPaper & 0x0F)));
}

bool Machine::ulaTransparent(u16 colour9) const {
  return isNext() && u8(colour9 >> 1) == next_.read(0x14);
}

bool Machine::ulaPlusEnabled() const {
  if (!isNext()) return false;
  return ula_.ulaPlusEnabled() || (next_.read(0x68) & 0x08) != 0;
}

u32 Machine::ulaBorderColour(u8 index) const {
  if (!isNext()) return Palette[index & 0x07];
  if (ula_.hiRes()) return paperColour(u8(8 + ula_.hiresColour()));
  if ((palette_.control() & 0x01) != 0) {
    return palette_.colour(ulaPalette(), u8(128 + (index & 0x07)));
  }
  if (ulaPlusEnabled()) return palette_.colour(ulaPalette(), 8);
  return paperColour(index);
}



void Machine::renderSpriteRow(int r) {
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
  // Resolving per row keeps sprite attribute writes made by the Copper on an
  // earlier scanline visible on the lines that follow them.
  const auto states = sprites_.resolve();

  const bool rowInScreen = r >= BorderHeight && r < BorderHeight + ScreenHeight;
  u32* row = framebuffer_.data() + size_t(r) * FrameWidth;
  const u8* occluders = occluders_.data() + size_t(r) * FrameWidth;
  spriteDrawn_.fill(false);

  // The hardware prepares sprites in list order and can only manage about a
  // hundred unscaled ones per scanline. Whatever it never reached is left out
  // of the line, and port 0x303B reports that the budget ran out.
  constexpr int SpritesPerLine = 100;
  int prepared = 0;

  for (int n = 0; n < Sprites::Count; ++n) {
    const int sprite = zeroOnTop ? Sprites::Count - 1 - n : n;
    const SpriteState& state = states[size_t(sprite)];
    if (!state.visible) continue;

    const int scaleX = state.scaleX;
    const int scaleY = state.scaleY;
    const int width = Sprites::Size * scaleX;
    const int height = Sprites::Size * scaleY;

    // Sprite coordinates wrap at 512, so a sprite hanging off the top or the
    // right edge reappears on the opposite side.
    const int dy = (r - state.y) & 0x1FF;
    if (dy >= height) continue;

    if (++prepared > SpritesPerLine) {
      sprites_.noteOverflow();
      continue;
    }

    const int offset = state.paletteOffset << 4;
    for (int dx = 0; dx < width; ++dx) {
      const int surfaceX = (state.x + dx) & 0x1FF;
      if (surfaceX >= FrameWidth) continue;

      // Sprites only reach into the border when over-border drawing is on.
      const bool inScreen = rowInScreen && surfaceX >= BorderWidth &&
                            surfaceX < BorderWidth + ScreenWidth;
      if (!inScreen && !overBorder) continue;

      // The compositor recorded the layers that sit above the sprites, so a
      // pixel of any of them hides the sprite.
      if (occluders[surfaceX] != 0) continue;

      // The clip window is measured from the ULA origin unless the sprites
      // reach into the border, where it moves to the sprite origin and its X
      // coordinates count double.
      if (!overBorder || clipOverBorder) {
        int clipX = overBorder ? surfaceX : surfaceX - BorderWidth;
        if (overBorder) clipX >>= 1;
        const int clipY = overBorder ? r : r - BorderHeight;
        if (clipX < clip[0] || clipX > clip[1]) continue;
        if (clipY < clip[2] || clipY > clip[3]) continue;
      }

      const int index =
          sprites_.pixel(state, dx / scaleX, dy / scaleY, transparency);
      if (index < 0) continue;

      // Two sprites over the same pixel are what the collision flag is for.
      if (spriteDrawn_[size_t(surfaceX)]) {
        sprites_.noteCollision();
      } else {
        spriteDrawn_[size_t(surfaceX)] = true;
      }

      row[surfaceX] = palette_.colour(palette, u8((offset + index) & 0xFF));
    }
  }
}

} // namespace rainbow
