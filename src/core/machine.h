#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/ay.h"
#include "core/beeper.h"
#include "core/bus.h"
#include "core/copper.h"
#include "core/dac.h"
#include "core/divmmc.h"
#include "core/dma.h"
#include "core/esxdos.h"
#include "core/expansion.h"
#include "core/keyboard.h"
#include "core/nextreg.h"
#include "core/palette.h"
#include "core/sdcard.h"
#include "core/sprites.h"
#include "core/turbosound.h"
#include "core/tape.h"
#include "core/upd765.h"
#include "core/types.h"
#include "core/ula.h"
#include "core/z80.h"

namespace rainbow {

namespace formats {
struct NexFile;
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
  bool contended = true;
};

const Timings& timingsFor(MachineType type);
const char* machineName(MachineType type);
// The short id a command line or a config file uses: `48`, `128`, `plus2`,
// `plus3`, `pentagon`, `next`. Returns nothing for an unknown id.
std::optional<MachineType> machineTypeFromId(const std::string& id);
// The id for a type, i.e. the inverse of machineTypeFromId.
const char* machineId(MachineType type);
u32 tstatesPerLine(const Timings& t);
u32 tstatesPerFrame(const Timings& t);

// Port reads can be intercepted. RZX replay works by handing the CPU the bytes
// the recording stored instead of the ones the machine would produce, so the
// hook sits at the outside of ioRead, past the expansion bus: every value a
// program can see passes through it.
class IoReadHook {
public:
  virtual ~IoReadHook() = default;
  virtual u8 ioRead(u16 port, u8 value) = 0;
};

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
  static constexpr int AltRomPages = 4;  // 32 KB, the two 0x8C alt banks

  explicit Machine(MachineType type = MachineType::Spectrum48);

  // A power-on reset also clears the RAM. A soft or hard reset triggered from
  // NextReg 0x02 leaves the SRAM contents intact, as on real hardware.
  void reset(bool preserveRam = false);

  // Brings a Next up the way its own firmware does. TBBLUE.FW does not load a
  // program: it configures the FPGA through the Next registers and then soft
  // resets, leaving NextZXOS to run from the card. Reproducing those writes is
  // the whole of the firmware, so the real boot ROM never has to run. Call it
  // once the ROM set is loaded; a hard reset does not repeat it.
  void nextFirmwareBoot();

  bool loadRom(const std::string& path);
  // Loads a multi-bank ROM image (128K, +2, +2A, +3). A 16K file fills one
  // bank, a 32K file fills the first two, a 64K file fills all four.
  bool loadRomImage(const std::string& path);
  bool loadRomImageData(const u8* data, size_t size);
  bool loadRomData(const u8* data, size_t size, int bank);
  bool loadSnapshot(const std::string& path, std::string& error);
  // Loads a .nex program. The machine must already be a Next; the caller
  // switches type first, because loading resets RAM and NextRegs.
  bool loadNex(const std::string& path, std::string& error);
  bool loadTape(const std::string& path, std::string& error);
  // +2A/+3 only: inserts a .dsk/.edsk image into the disk controller.
  bool loadDisk(const std::string& path, std::string& error);
  void ejectDisk() { disk_ = formats::DiskImage{}; fdc_.eject(); }

  // Runs one frame. With breakpoints armed the run stops before the
  // instruction at the breakpoint executes, so the registers still show the
  // state to inspect, and the address that stopped it is returned.
  std::optional<u16> runFrame();
  void runCycles(u32 cycles);
  void stepInstruction();

  // --- debugger -----------------------------------------------------------
  // Breakpoints are addresses; the check costs nothing while none are armed.
  void setBreakpoint(u16 address);
  void clearBreakpoint(u16 address);
  bool hasBreakpoint(u16 address) const;
  void clearBreakpoints();
  const std::vector<u16>& breakpoints() const { return breakpoints_; }

  // PC sampling for the profiler. Off by default because it costs a store per
  // instruction, so a frontend only turns it on while someone is looking.
  void setProfiling(bool on);
  bool profiling() const { return profiling_; }
  void resetProfile();
  // How often an instruction started at `address` since the last reset.
  u32 sampleCount(u16 address) const { return samples_[address]; }
  u64 sampleTotal() const { return sampleTotal_; }

  // --- save states --------------------------------------------------------
  // Everything that makes the machine what it is, as a versioned little-endian
  // stream with a checksum. Tapes, disks and the SD card image are not embedded
  // -- they are large, user-supplied files -- so a state restores where they
  // were and the frontend keeps them attached.
  void saveState(std::vector<u8>& out);
  bool loadState(const std::vector<u8>& in, std::string& error);
  // Reads only the header, so a frontend can switch to the machine a state was
  // written for before asking it to load. Says nothing about the rest of the
  // file: loadState still checks everything.
  static bool peekStateType(const std::vector<u8>& in, MachineType& type);
  // Applies an already-decoded snapshot. An RZX file carries its snapshot
  // inside itself, so it cannot go through loadSnapshot()'s file dispatch.
  void applySnapshotData(const formats::Snapshot& snapshot) { applySnapshot(snapshot); }

  // Bus interface
  u8 memRead(u16 addr, u32 t) override;
  void memWrite(u16 addr, u8 value, u32 t) override;
  u8 memFetch(u16 addr, u32 t) override;
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
  // Headless runs (conformance tests, batch tools) skip the per-pixel
  // compositor. Timing, interrupts and the border log still advance, so the
  // emulated program cannot tell the difference.
  void setVideoEnabled(bool on) { videoEnabled_ = on; }
  bool videoEnabled() const { return videoEnabled_; }
  int frameWidth() const { return FrameWidth; }
  int frameHeight() const { return FrameHeight; }
  u64 frames() const { return frames_; }
  // Instructions executed since the machine was built. An RZX frame records how
  // many it ran, and only a frame boundary reads the counter.
  u64 instructions() const { return instructions_; }
  // A recording needs to see, and a replay needs to replace, every port read.
  void setIoReadHook(IoReadHook* hook) { ioReadHook_ = hook; }
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
  // The first AY is the one every machine with a sound chip has; the other two
  // only run on the Next.
  Ay38912& ay() { return sound_.chip(0); }
  const Ay38912& ay() const { return sound_.chip(0); }
  TurboSound& sound() { return sound_; }
  const TurboSound& sound() const { return sound_; }
  const SpecDrum& dacs() const { return dacs_; }
  bool is128k() const { return type_ != MachineType::Spectrum48; }
  bool isPlus3() const { return type_ == MachineType::Plus3; }
  bool irqLine() const { return irqLine_; }
  bool romLoaded() const { return romLoaded_; }
  void setKempston(u8 value) { kempston_ = value; }
  // The edge connector: peripherals are plugged in here, and the settings menu
  // switches them on and off through it.
  ExpansionBus& expansion() { return expansion_; }
  const ExpansionBus& expansion() const { return expansion_; }
  // The DOS a Next program talks to through `RST 8`. It lives in the machine
  // rather than on the bus because the trap happens before the instruction is
  // fetched, which is earlier than any peripheral gets a look at it.
  EsxDos& esxDos() { return esxDos_; }
  const EsxDos& esxDos() const { return esxDos_; }
  // The Next's own SD card sits on the SPI bus at ports 0xE7 (chip select) and
  // 0xEB (data). It belongs to the machine rather than to a peripheral because
  // the boot ROM drives it directly, before any module could be mapped in.
  SdCard& sdCard() { return sdCard_; }
  const SdCard& sdCard() const { return sdCard_; }
  // The divMMC block. On the Next it is part of the FPGA, not a cartridge: the
  // boot ROM writes port 0xE3 and refuses to continue unless the divMMC ROM
  // answers with its "DV" signature, so the machine always owns one.
  DivMmc& divMmc() { return divmmc_; }
  const DivMmc& divMmc() const { return divmmc_; }
  // Loads the 8K esxDOS-style ROM the boot ROM expects at 0x0000.
  bool loadDivMmcRom(const std::string& path, std::string& error);
  const NextRegFile& nextRegs() const { return next_; }
  const NextPalette& palette() const { return palette_; }
  const Sprites& sprites() const { return sprites_; }
  const Copper& copper() const { return copper_; }
  const ZxnDma& dma() const { return dma_; }
  u8 readNextReg(u8 reg) const;
  void writeNextReg(u8 reg, u8 value);
  u32 tstatesPerLineValue() const { return tstatesPerLine_; }
  u32 tstatesPerFrameValue() const { return tstatesPerFrame_; }

  void setSampleRate(int hz);
  const std::vector<float>& audioBuffer() const { return audio_; }
  void clearAudio() { audio_.clear(); }

  static constexpr u32 Transparent = 0x00000000;
  static constexpr int PaletteSize = 16;
  static const u32 Palette[PaletteSize];

private:
  // The machine's own port decode. The public ioRead/ioWrite offer the access to
  // the expansion bus first and fall back to these, which is also what a
  // peripheral's host callbacks reach, so a module never recurses into itself.
  u8 ioReadBase(u16 port, u32 t);
  void ioWriteBase(u16 port, u8 value, u32 t);
  // Same split for memory: the public memRead/memWrite give the expansion bus
  // first refusal, so a module can shadow ROM or RAM, and fall back to these.
  u8 memReadBase(u16 addr, u32 t);
  void memWriteBase(u16 addr, u8 value, u32 t);
  void installPeripherals();
  // Push NextRegs 0x0A and 0xB8-0xBB into the divMMC module.
  void syncDivMmcAutomap();
  void refreshCartridge();

  void renderFrame();
  void renderScreenRow(int y);
  void frameStart();
  u8 floatingBus() const;
  void writePort7ffd(u8 value);
  void writePort1ffd(u8 value);
  void updatePaging();
  void updateNextMmu();
  u8 nextRomBank() const;
  u8 nextAltRomBank() const;
  bool nextAltRomPage(u16 addr, bool write, u8& page) const;
  bool nextDivMmcRom3() const;
  u32 turboMultiplier() const;
  // Layer 2 state, resolved once per scanline because the fetch runs per pixel.
  struct Layer2View {
    bool active = false;
    bool fourBit = false;
    int width = 0;
    int height = 0;
    int scrollX = 0;
    int scrollY = 0;
    int palette = 0;
    u8 transparency = 0;
    int clipX1 = 0;
    int clipX2 = 255;
    int clipY1 = 0;
    int clipY2 = 191;
    size_t base = 0;
  };
  Layer2View layer2View() const;
  // One framebuffer pixel of Layer 2, or Transparent when nothing is drawn.
  // priority reports bit 7 of the colour, which the blending modes propagate so
  // that the pixel ends up above the sprites.
  u32 layer2Pixel(const Layer2View& view, int x, int y, bool& priority) const;
  // The tilemap shares the ULA's display layer and covers the whole surface,
  // border included, so it is resolved per pixel next to the ULA.
  struct TilemapView {
    bool active = false;
    bool textMode = false;
    bool noAttributes = false;
    bool fiveTwelve = false;
    bool tilemapOverUla = false;
    bool stencil = false;
    int columns = 40;
    int scrollX = 0;
    int scrollY = 0;
    int palette = 0;
    u8 transparency = 0x0F;
    u8 defaultAttribute = 0;
    size_t mapBank = 0;
    size_t mapOffset = 0;
    size_t mapMask = 0x3FFF;
    size_t defBank = 0;
    size_t defOffset = 0;
    size_t defMask = 0x3FFF;
    std::array<u8, 4> clip = {0, 159, 0, 255};
  };
  TilemapView tilemapView() const;
  // One tilemap pixel, or Transparent. ulaOver reports the tile's own
  // "ULA on top" attribute, which the ULA needs to combine with the tilemap.
  u32 tilemapPixel(const TilemapView& view, int x, int y, bool& ulaOver) const;
  // The ULA clip window is inclusive on all four edges.
  bool ulaClipped(int x, int y) const;
  void renderUlaRow(int y, std::array<u32, ScreenWidth>& row) const;
  // Timex hi-res: 512 pixels per line, eight bits from the screen file and eight
  // from the file 8K above it. The framebuffer is 256 wide, so each output pixel
  // is the average of the two hi-res pixels it covers.
  void renderUlaHiresRow(int y, std::array<u32, ScreenWidth>& row) const;
  // LoRes replaces the ULA with a 128x96 image stretched over the whole 256x192
  // viewport, two display pixels per dot. NextReg 0x15 bit 7 switches it on and
  // 0x6A bit 5 turns it into Radastan, four bits per dot.
  bool loresEnabled() const;
  bool radastanMode() const;
  void renderLoresRow(int y, std::array<u32, ScreenWidth>& row) const;
  // Register 0x15 orders sprites, Layer 2 and the ULA from top to bottom.
  std::array<int, 3> layerOrder() const;
  static int layerDepth(const std::array<int, 3>& order, int layer);
  // The raster line the framebuffer row `r` belongs to, border included.
  int rasterLineFor(int r) const;
  // Sprites are drawn after the row's layers, because they may sit above or
  // below them and may reach into the border.
  void renderSpriteRow(int r);
  // The colour the ULA draws with, honouring the Next palette when present.
  u32 ulaColour(u8 index) const;
  u32 paperColour(u8 index) const;
  int ulaPalette() const;
  // ULA+ is switched on either by its own mode register or by NextReg 0x68.
  bool ulaPlusEnabled() const;
  // The ink and paper colours of one attribute cell. ULA+ and ULANext remap the
  // palette indices, so both halves are resolved together. The 9-bit palette
  // values come back too, because NextReg 0x14 compares against them and not
  // against the RGB the palette expands to.
  void ulaCellColours(u8 attr, bool flashOn, u32& ink, u32& paper,
                      u16& ink9, u16& paper9) const;
  // True when a 9-bit palette value is the transparent one. Only the top eight
  // bits take part, so 0x1C7 is transparent when 0x14 holds 0xE3.
  bool ulaTransparent(u16 colour9) const;
  u32 ulaBorderColour(u8 index) const;
  // Maps one 16K bank into a pair of neighbouring 8K slots.
  void mapBank(int slot, int bank, bool ram);
  void applySnapshot(const formats::Snapshot& snapshot);
  void applyNex(const formats::NexFile& nex);
  bool applyTapeFastLoad(u16 entry);

  // The body of a save state, shared by both directions. Defined in
  // machine.cpp and instantiated for the reader and the writer, so the two can
  // never disagree about the order or the set of fields.
  template <class Ar> void serialize(Ar& ar);

  MachineType type_;
  Timings timings_;
  u32 tstatesPerLine_ = 224;
  u32 tstatesPerFrame_ = 69888;

  Z80 cpu_;
  EsxDos esxDos_;
  SdCard sdCard_;
  DivMmc divmmc_;
  // Port 0xE7 latch. Every device on the bus is selected by clearing its bit,
  // so the reset value leaves all of them idle.
  u8 spiSelect_ = 0xFF;
  Ula ula_;
  Keyboard keyboard_;
  Beeper beeper_;
  TurboSound sound_;
  SpecDrum dacs_;
  Tape tape_;
  Upd765 fdc_;
  formats::DiskImage disk_;
  NextRegFile next_;
  NextPalette palette_;
  Sprites sprites_;
  Copper copper_;
  ZxnDma dma_;
  ExpansionBus expansion_;
  // The cartridge image, cached from the bus so the memory path stays a single
  // pointer test.
  const std::vector<u8>* cartRom_ = nullptr;
  // Register 0x18 takes four writes in a row, so the position needs a counter.
  std::array<u8, 4> layer2Clip_ = {0, 255, 0, 191};
  u8 layer2ClipIndex_ = 0;
  // Register 0x1B does the same for the tilemap.
  std::array<u8, 4> tilemapClip_ = {0, 159, 0, 255};
  u8 tilemapClipIndex_ = 0;
  // And register 0x1A for the ULA (and LoRes, which shares its layer).
  std::array<u8, 4> ulaClip_ = {0, 255, 0, 191};
  u8 ulaClipIndex_ = 0;
  // Bit per pixel: the layers that hide a sprite at that position.
  std::vector<u8> occluders_;
  // One scanline of the collision map: a sprite has already drawn there.
  std::array<bool, FrameWidth> spriteDrawn_{};

  std::vector<u8> rom_;
  // The two 16K alt ROM banks behind the main ROM (register 0x8C). NextZXOS
  // copies enAltZX.rom in here at boot and switches to it for +3 mode.
  std::vector<u8> altRom_;
  std::vector<u8> ram_;
  std::vector<u32> framebuffer_;
  std::array<u8, 400> borderLines_{};

  u8 ramBank_ = 0;
  u8 screenBank_ = 5;
  u8 romBank_ = 0;
  u8 port7ffd_ = 0;
  u8 port1ffd_ = 0;
  // Port 0x123B bit 1, the legacy Layer 2 enable. Only bit 1 is modelled,
  // because the CPU mapping the rest of the port selects is not wired up.
  u8 port123b_ = 0;
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

  // tstates_ is the ULA clock and cpuTstates_ the CPU clock; they diverge on
  // the Next, where the CPU can run several times faster than the ULA.
  u64 tstates_ = 0;
  u64 cpuTstates_ = 0;
  u32 turboAccum_ = 0;
  u64 frames_ = 0;
  // Monotonic, so runFrame() can tell that the guest reset the machine.
  u64 resets_ = 0;
  u64 instructions_ = 0;
  IoReadHook* ioReadHook_ = nullptr;
  bool irqLine_ = false;
  u32 irqEnd_ = 0;

  std::vector<u16> breakpoints_;  // sorted, so the per-instruction test is a search
  bool profiling_ = false;
  bool videoEnabled_ = true;
  std::array<u32, 65536> samples_{};
  u64 sampleTotal_ = 0;

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

} // namespace rainbow
