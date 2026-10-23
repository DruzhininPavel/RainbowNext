#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace rainbow {

// The divMMC is not a cartridge on the Next: it is a block inside the FPGA, so
// the machine owns one the same way it owns the ULA. Two things make it more
// than a device hanging off a port.
//
// First, memory. When it is mapped in it replaces the bottom 8K with its own
// ROM and the next 8K with one of its RAM banks, and it maps itself in on its
// own when the CPU fetches an opcode at one of the hook addresses -- which is
// why the machine has to offer an M1 cycle at all.
//
// Second, the boot handshake. The Next's boot ROM writes port 0xE3, then checks
// that 0x0004 reads "DV", and hangs forever at 0x1ED9 if it does not. That
// signature only exists in the divMMC ROM, so NextZXOS cannot start without
// this block.
//
// The card is not here. On the Next the SPI interface is a separate device with
// its own ports, and `SdCard` in the machine serves it; divMMC only supplies the
// memory window and the control latch.
class DivMmc {
public:
  static constexpr u16 PortControl = 0xE3;

  static constexpr u32 RomSize = 0x2000;   // 8K of esxDOS
  static constexpr u32 BankSize = 0x2000;  // 8K per RAM bank
  static constexpr int BankCount = 16;     // 128K of divMMC RAM

  // The control latch, port 0xE3.
  static constexpr u8 Conmem = 0x80;  // mapped in
  static constexpr u8 Mapram = 0x40;  // bank 3 shadows the ROM at 0x0000

  // A hard reset drops the latch and the RAM; the reset button only drops the
  // latch, because esxDOS keeps warm-boot markers in the RAM.
  void hardReset();
  void softReset();

  bool loadRom(std::vector<u8> rom, std::string& error);
  bool loadRomFile(const std::string& path, std::string& error);
  bool romLoaded() const { return !rom_.empty(); }
  void unloadRom() { rom_.clear(); control_ = 0; }

  u8 control() const { return control_; }
  void setControl(u8 value) { control_ = value; }
  bool mappedIn() const { return (control_ & Conmem) != 0; }
  bool mapram() const { return (control_ & Mapram) != 0; }
  int bank() const { return control_ & 0x0F; }
  // A single 8K image has one page; a 32K image carries the four esxDOS pages
  // and the top two bits of the latch pick one.
  u32 romPage() const;
  u32 romPages() const { return rom_.empty() ? 0 : u32(rom_.size() / RomSize); }
  const std::vector<u8>& ram() const { return ram_; }

  // Automap is a board option rather than part of the latch: it decides whether
  // an opcode fetch at a hook address pulls the block in by itself. The Next
  // exposes it as NextReg 0x0A bit 4 and leaves it off, so its boot ROM keeps
  // the RST 0/8 vectors until NextZXOS hands them to esxDOS.
  void setAutomap(bool on) { automap_ = on; }
  bool automap() const { return automap_; }
  // Which addresses automap, and under what conditions. NextRegs 0xB8-0xBB:
  // 0xB8/0xBB hold one enable bit per entry point, 0xB9 says whether a point
  // fires everywhere or only while the +3 DOS ROM is paged in, and 0xBA asks
  // for the map to happen after the instruction rather than before it. Mapping
  // before the fetch is what makes a hook address work at all, so the timing
  // bits are accepted but not acted on.
  void setEntryPoints(u8 points0, u8 valid0, u8 timing0, u8 points1) {
    entryPoints0_ = points0;
    entryPointsValid0_ = valid0;
    entryPointsTiming0_ = timing0;
    entryPoints1_ = points1;
  }
  u8 entryPoints0() const { return entryPoints0_; }
  u8 entryPointsValid0() const { return entryPointsValid0_; }
  u8 entryPointsTiming0() const { return entryPointsTiming0_; }
  u8 entryPoints1() const { return entryPoints1_; }
  // The machine answers whether the +3 DOS ROM is the one at 0x0000.
  void setRom3Present(bool present) { rom3Present_ = present; }

  // Whether an opcode fetch at `addr` pulls the block in, entry points and the
  // ROM3 condition included. The automap master switch is not consulted.
  bool hookEnabled(u16 addr) const;

  bool ioRead(u16 port, u8& value) const;
  bool ioWrite(u16 port, u8 value);

  // Returns true when the access belongs to the divMMC window. `a` is the
  // accumulator, which the 0x1FF8 unmap stubs test.
  bool read(u16 addr, bool m1, u8 a, u8& value);
  bool write(u16 addr, u8 value);
  // The unmapping stubs at 0x1FF8-0x1FFF take effect once the instruction that
  // fetched them has finished, which is what this closes out.
  void afterInstruction();

private:
  std::vector<u8> rom_;
  std::vector<u8> ram_ = std::vector<u8>(size_t(BankCount) * BankSize, 0);
  u8 control_ = 0;
  bool automap_ = false;
  bool rom3Present_ = false;
  bool unmapPending_ = false;
  u8 entryPoints0_ = 0x83;
  u8 entryPointsValid0_ = 0x01;
  u8 entryPointsTiming0_ = 0x00;
  u8 entryPoints1_ = 0xCD;
};

} // namespace rainbow
