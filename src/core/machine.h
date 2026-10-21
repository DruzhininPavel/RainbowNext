#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/ay.h"
#include "core/beeper.h"
#include "core/bus.h"
#include "core/keyboard.h"
#include "core/nextreg.h"
#include "core/tape.h"
#include "core/upd765.h"
#include "core/types.h"
#include "core/ula.h"
#include "core/z80.h"

namespace mynext {

namespace formats {
struct Snapshot;
}

enum class MachineType {
  Spectrum48,
  Spectrum128,
  Plus2,
  Plus3,
  Pentagon,
  Next,
};

struct Timings {
  u32 cpuHz = 3500000;
  u32 ayHz = 0;
  u16 leftBorder = 24;
  u16 horizontalScreen = 128;
  u16 rightBorder = 24;
  u16 horizontalRetrace = 48;
  u16 topBorder = 48;
  u16 verticalScreen = 192;
  u16 bottomBorder = 48;
  u16 verticalRetrace = 24;
  u16 interruptLength = 32;
  u16 topLeftPixel = 14336;
  bool contended = true;
};

const Timings& timingsFor(MachineType type);
const char* machineName(MachineType type);
u32 tstatesPerLine(const Timings& t);
u32 tstatesPerFrame(const Timings& t);

// The complete machine: CPU, ULA, memory, paging and video. The class owns the
// clock, so every subsystem observes the same T-state counter.
class Machine : public Bus {
public:
  static constexpr int ScreenWidth = 256;
  static constexpr int ScreenHeight = 192;
  static constexpr int BorderWidth = 32;
  static constexpr int BorderHeight = 32;
  static constexpr int FrameWidth = ScreenWidth + 2 * BorderWidth;
  static constexpr int FrameHeight = ScreenHeight + 2 * BorderHeight;
  // The classic machines and the snapshots speak in 16K banks; the Next maps
  // the same memory as 8K pages through its MMU. One table of 8K slots serves
  // both, and a 16K bank is simply two neighbouring pages.
  static constexpr int RamBanks = 8;
  static constexpr int RomBanks = 4;
  static constexpr int BankSize = 0x4000;
  static constexpr int PageSize = 0x2000;
  static constexpr int RamPages = 256; // 2 MB
  static constexpr int RomPages = 8;   // 64 KB

  explicit Machine(MachineType type = MachineType::Spectrum48);

  void reset();

  bool loadRom(const std::string& path);
  // Loads a multi-bank ROM image (128K, +2, +2A, +3). A 16K file fills one
  // bank, a 32K file fills the first two, a 64K file fills all four.
  bool loadRomImage(const std::string& path);
  bool loadRomImageData(const u8* data, size_t size);
  bool loadRomData(const u8* data, size_t size, int bank);
  bool loadSnapshot(const std::string& path, std::string& error);
  bool loadTape(const std::string& path, std::string& error);
  // +2A/+3 only: inserts a .dsk/.edsk image into the disk controller.
  bool loadDisk(const std::string& path, std::string& error);
  void ejectDisk() { disk_ = formats::DiskImage{}; fdc_.eject(); }

  void runFrame();
  void runCycles(u32 cycles);
  void stepInstruction();

  // Bus interface
  u8 memRead(u16 addr, u32 t) override;
  void memWrite(u16 addr, u8 value, u32 t) override;
  u8 ioRead(u16 port, u32 t) override;
  void ioWrite(u16 port, u8 value, u32 t) override;
  u8 contention(u16 addr, u32 t) override;
  void tick(u32 cycles) override;
  u32 now() const override { return u32(tstates_ % tstatesPerFrame_); }
  u8 interruptVector() override { return 0xFF; }
  void nextRegWrite(u8 reg, u8 value) override { writeNextReg(reg, value); }

  // Accessors
  MachineType type() const { return type_; }
  const Timings& timings() const { return timings_; }
  Z80& cpu() { return cpu_; }
  const Z80& cpu() const { return cpu_; }
  Keyboard& keyboard() { return keyboard_; }
  Tape& tape() { return tape_; }
  const Tape& tape() const { return tape_; }
  const u32* framebuffer() const { return framebuffer_.data(); }
  int frameWidth() const { return FrameWidth; }
  int frameHeight() const { return FrameHeight; }
  u64 frames() const { return frames_; }
  u8 border() const { return ula_.border(); }
  u8 ramBank() const { return ramBank_; }
  u8 screenBank() const { return screenBank_; }
  u8 romBank() const { return romBank_; }
  u8 port7ffd() const { return port7ffd_; }
  u8 port1ffd() const { return port1ffd_; }
  bool pagingLocked() const { return pagingLocked_; }
  bool specialPaging() const { return specialPaging_; }
  bool isNext() const { return type_ == MachineType::Next; }
  bool hasAy() const { return type_ != MachineType::Spectrum48; }
  bool hasDiskController() const { return type_ == MachineType::Plus3; }
  Upd765& fdc() { return fdc_; }
  const Upd765& fdc() const { return fdc_; }
  const formats::DiskImage& disk() const { return disk_; }
  Ay38912& ay() { return ay_; }
  const Ay38912& ay() const { return ay_; }
  bool is128k() const { return type_ != MachineType::Spectrum48; }
  bool isPlus3() const { return type_ == MachineType::Plus3; }
  bool irqLine() const { return irqLine_; }
  bool romLoaded() const { return romLoaded_; }
  void setKempston(u8 value) { kempston_ = value; }
  const NextRegFile& nextRegs() const { return next_; }
  u8 readNextReg(u8 reg) const;
  void writeNextReg(u8 reg, u8 value);
  u32 tstatesPerLineValue() const { return tstatesPerLine_; }
  u32 tstatesPerFrameValue() const { return tstatesPerFrame_; }

  void setSampleRate(int hz);
  const std::vector<float>& audioBuffer() const { return audio_; }
  void clearAudio() { audio_.clear(); }

  static constexpr int PaletteSize = 16;
  static const u32 Palette[PaletteSize];

private:
  void renderFrame();
  void renderScreenRow(int y);
  void frameStart();
  u8 floatingBus() const;
  void writePort7ffd(u8 value);
  void writePort1ffd(u8 value);
  void updatePaging();
  void updateNextMmu();
  // Maps one 16K bank into a pair of neighbouring 8K slots.
  void mapBank(int slot, int bank, bool ram);
  void applySnapshot(const formats::Snapshot& snapshot);
  bool applyTapeFastLoad();

  MachineType type_;
  Timings timings_;
  u32 tstatesPerLine_ = 224;
  u32 tstatesPerFrame_ = 69888;

  Z80 cpu_;
  Ula ula_;
  Keyboard keyboard_;
  Beeper beeper_;
  Ay38912 ay_;
  Tape tape_;
  Upd765 fdc_;
  formats::DiskImage disk_;
  NextRegFile next_;

  std::vector<u8> rom_;
  std::vector<u8> ram_;
  std::vector<u32> framebuffer_;
  std::array<u8, 400> borderLines_{};

  u8 ramBank_ = 0;
  u8 screenBank_ = 5;
  u8 romBank_ = 0;
  u8 port7ffd_ = 0;
  u8 port1ffd_ = 0;
  bool pagingLocked_ = false;
  bool specialPaging_ = false;
  bool romLoaded_ = false;

  // Resolved memory map: one entry per 16K slot, so the +2A/+3's four CP/M
  // configurations need no special cases in the read and write paths.
  struct Page {
    u16 page = 0;
    bool ram = false;
  };
  std::array<Page, 8> page_{};

  u64 tstates_ = 0;
  u64 frames_ = 0;
  bool irqLine_ = false;
  u32 irqEnd_ = 0;

  int sampleRate_ = 44100;
  u64 audioAccum_ = 0;
  // Interleaved stereo, so the +2A/+3's ABC wiring is audible.
  std::vector<float> audio_;
  float dcInL_ = 0.0f;
  float dcOutL_ = 0.0f;
  float dcInR_ = 0.0f;
  float dcOutR_ = 0.0f;

  u8 kempston_ = 0;
};

} // namespace mynext
