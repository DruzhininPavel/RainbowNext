#pragma once

#include <string>
#include <vector>

#include "core/peripheral.h"
#include "core/sdcard.h"

namespace rainbow {

// The DivMMC: the mass-storage interface esxDOS runs on. Two things make it a
// peripheral rather than a device hanging off a port.
//
// First, memory. When it is mapped in it replaces the bottom 8K with its own
// ROM and the next 8K with one of its RAM banks, and it maps itself in on its
// own when the CPU fetches an opcode at one of the hook addresses -- which is
// why the module contract has an M1 cycle at all.
//
// Second, the card. The esxDOS ROM that the user drops in talks to an SD card
// over SPI through four ports, so the module carries both halves and neither
// the machine nor the frontend needs to know that a filesystem is involved. The
// card itself is `SdCard` from the core, so the same state machine serves the
// Next's built-in interface and anything else that needs one.
class DivMmc final : public Peripheral {
public:
  static constexpr u16 PortControl = 0xE3;
  static constexpr u16 PortUnmap = 0xE7;
  static constexpr u16 PortData = 0xEB;
  // The original DivIDE put the SPI byte on 0xDF; the Next moved it to 0xEB and
  // handed 0xDF to the Kempston joystick and DAC D, so the legacy port is only
  // decoded on a classic machine.
  static constexpr u16 PortSpiData = 0xDF;
  // The map-in latch of the older boards sits on the same port as the Next's
  // data port; which of the two a write means depends on the host.
  static constexpr u16 PortMapIn = PortData;
  // The status port is decoded on four mirrors so a driver can pick one that
  // does not collide with whatever else is plugged in.
  static constexpr u16 PortStatus = 0x1F;

  static constexpr u32 RomSize = 0x2000;   // 8K of esxDOS
  static constexpr u32 BankSize = 0x2000;  // 8K per RAM bank
  static constexpr int BankCount = 16;     // 128K of DivMMC RAM

  // The control latch, port 0xE3.
  static constexpr u8 Conmem = 0x80;  // mapped in
  static constexpr u8 Mapram = 0x40;  // bank 3 shadows the ROM at 0x0000

  const PeripheralInfo& info() const override;
  void reset() override;
  void enable() override;
  void disable() override;

  bool ioRead(u16 port, u32 t, u8& value) override;
  bool ioWrite(u16 port, u8 value, u32 t) override;

  // The bus only offers memory while this is true, and it is true whenever the
  // module is plugged in: whether an access is claimed depends on the latch, so
  // it is decided per access rather than once.
  bool interceptsMemory() const override { return true; }
  bool memRead(u16 addr, u32 t, bool m1, u8& value) override;
  bool memWrite(u16 addr, u8 value, u32 t) override;

  // One entry point for two different images: the extension decides which.
  // `.rom` is the esxDOS ROM, anything else is a sector image for the card.
  const char* fileFilter() const override;
  bool loadFile(const std::string& path, std::string& error) override;

  // --- for the frontend and the tests ---
  bool loadRom(std::vector<u8> rom, std::string& error);
  bool loadCard(std::vector<u8> image, std::string& error);
  // The explicit pair, for a menu that already knows which of the two the user
  // asked for and must not guess from the extension.
  bool loadRomFile(const std::string& path, std::string& error);
  bool loadCardFile(const std::string& path, std::string& error);
  bool romLoaded() const { return !rom_.empty(); }
  u8 control() const { return control_; }
  void setControl(u8 value) { control_ = value; }
  // Automap is a board option rather than part of the latch: it decides whether
  // an opcode fetch at a hook address pulls the module in by itself. The Next
  // exposes it as NextReg 0x0A bit 4 and leaves it off, so its boot ROM keeps
  // the RST 0/8 vectors until NextZXOS hands them to esxDOS.
  void setAutomap(bool on) { automap_ = on; }
  bool automap() const { return automap_; }
  // Whether the DivIDE-era 0xDF data port is decoded. Off on the Next, where
  // 0xDF belongs to the Kempston joystick and DAC D; the data port there is
  // 0xEB, which is always decoded.
  void setLegacySpiPort(bool on) { legacySpiPort_ = on; }
  bool legacySpiPort() const { return legacySpiPort_; }
  bool mappedIn() const { return (control_ & Conmem) != 0; }
  bool mapram() const { return (control_ & Mapram) != 0; }
  int bank() const { return control_ & 0x0F; }
  SdCard& card() { return card_; }
  const SdCard& card() const { return card_; }
  // The 8K page the ROM region shows. A 32K image carries four esxDOS pages and
  // the top two bits of the latch pick one; a single 8K image has only page 0.
  u32 romPage() const;
  u32 romPages() const { return rom_.empty() ? 0 : u32(rom_.size() / RomSize); }
  const std::vector<u8>& ram() const { return ram_; }

  // The addresses whose M1 fetch maps the module in. 0x0066 is the NMI vector
  // and 0x04C6/0x0562 are esxDOS's own hooks; 0x0000/0x0008/0x0038 are there
  // because DivIDE maps in on reset and on the two RST vectors it uses.
  static bool isHookAddress(u16 addr);

private:
  PeripheralInfo info_;
  std::vector<u8> rom_;
  std::vector<u8> ram_ = std::vector<u8>(size_t(BankCount) * BankSize, 0);
  u8 control_ = 0;
  bool automap_ = true;
  bool legacySpiPort_ = true;
  SdCard card_;
  // The SPI port is one register: a write clocks a byte through the card and
  // latches what came back, a read hands that latched byte over without
  // clocking again. Drivers are written as `out (0xDF),a : in a,(0xDF)`, so
  // anything else would run the card at twice the clock rate.
  u8 spiLatch_ = 0xFF;
};

} // namespace rainbow
