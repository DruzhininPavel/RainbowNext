#include "test_framework.h"

#include "core/machine.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;
using rainbow::tstatesPerLine;
using rainbow::tstatesPerFrame;

namespace {

// A T-state inside the visible part of the screen, where contention happens.
u32 visibleTstate(const Machine& m) {
  const auto& t = m.timings();
  return tstatesPerLine(t) * (u32(t.topBorder) + 10) + u32(t.leftBorder) + 8;
}

void loadFourRomBanks(Machine& m) {
  for (int bank = 0; bank < 4; ++bank) {
    u8 page[0x4000];
    for (size_t i = 0; i < sizeof page; ++i) page[i] = u8(bank * 16 + (i & 0x0F));
    m.loadRomData(page, sizeof page, bank);
  }
}

} // namespace

TEST(k128_paging_switches_the_high_bank) {
  Machine m(MachineType::Spectrum128);

  m.memWrite(0xC000, 0x11, 0); // bank 0
  m.ioWrite(0x7FFD, 0x01, 0);  // bank 1 at 0xC000
  CHECK_EQ(m.ramBank(), 1);
  m.memWrite(0xC000, 0x22, 0);

  m.ioWrite(0x7FFD, 0x00, 0);
  CHECK_EQ(m.memRead(0xC000, 0), 0x11);
  m.ioWrite(0x7FFD, 0x01, 0);
  CHECK_EQ(m.memRead(0xC000, 0), 0x22);

  // Bank 5 is fixed at 0x4000 and bank 2 at 0x8000 whatever is paged in.
  m.memWrite(0x4000, 0x55, 0);
  m.memWrite(0x8000, 0x22, 0);
  m.ioWrite(0x7FFD, 0x03, 0);
  CHECK_EQ(m.memRead(0x4000, 0), 0x55);
  CHECK_EQ(m.memRead(0x8000, 0), 0x22);
}

TEST(k128_paging_lock_freezes_the_mapping) {
  Machine m(MachineType::Spectrum128);
  m.ioWrite(0x7FFD, 0x20 | 0x03, 0); // lock, bank 3
  CHECK(m.pagingLocked());
  CHECK_EQ(m.ramBank(), 3);

  m.ioWrite(0x7FFD, 0x05, 0);
  CHECK_EQ(m.ramBank(), 3);
  CHECK_EQ(m.port7ffd(), 0x23);
}

TEST(k128_screen_bank_follows_bit_three) {
  Machine m(MachineType::Spectrum128);
  CHECK_EQ(m.screenBank(), 5);
  m.ioWrite(0x7FFD, 0x08, 0);
  CHECK_EQ(m.screenBank(), 7);
  m.ioWrite(0x7FFD, 0x00, 0);
  CHECK_EQ(m.screenBank(), 5);
}

// Bit 3 swaps the whole 16 KiB window at 0x4000, not just the bank the ULA
// reads its pixels from. Games that unpack a level into the shadow screen rely
// on seeing the other bank's bytes there.
TEST(k128_screen_bit_leaves_the_4000_window_on_bank_five) {
  Machine m(MachineType::Spectrum128);

  m.ioWrite(0x7FFD, 0x05, 0); // bank 5 at 0xC000, screen bank 5
  m.memWrite(0xC000, 0xAA, 0);
  m.ioWrite(0x7FFD, 0x07, 0); // bank 7 at 0xC000, screen bank 5
  m.memWrite(0xC000, 0xBB, 0);

  m.ioWrite(0x7FFD, 0x05, 0);
  CHECK_EQ(m.memRead(0x4000, 0), 0xAA);
  // Bit 3 only changes the bank the ULA draws from; 0x4000 stays on bank 5.
  m.ioWrite(0x7FFD, 0x0D, 0);
  CHECK_EQ(m.screenBank(), 7);
  CHECK_EQ(m.memRead(0x4000, 0), 0xAA);
  CHECK_EQ(m.memRead(0xC000, 0), 0xAA);

  // Variables a program keeps at 0x7F00..0x7FFF must survive selecting the
  // shadow screen, because they live in the fixed bank 5 window.
  m.memWrite(0x7FA5, 0xDF, 0);
  m.ioWrite(0x7FFD, 0x1F, 0); // bank 7 at 0xC000, screen bank 7, ROM 1
  CHECK_EQ(m.screenBank(), 7);
  CHECK_EQ(m.memRead(0x7FA5, 0), 0xDF);
  CHECK_EQ(m.memRead(0xC000, 0), 0xBB);
}

TEST(k128_rom_bank_follows_bit_four) {
  Machine m(MachineType::Spectrum128);
  loadFourRomBanks(m);

  CHECK_EQ(m.memRead(0x0000, 0), 0x00);
  m.ioWrite(0x7FFD, 0x10, 0);
  CHECK_EQ(m.romBank(), 1);
  CHECK_EQ(m.memRead(0x0000, 0), 0x10);

  // The 128K has only two ROM pages; bit 4 is the whole selection.
  m.ioWrite(0x7FFD, 0x00, 0);
  CHECK_EQ(m.memRead(0x0000, 0), 0x00);
}

TEST(k128_plus3_rom_bank_uses_the_1ffd_high_bit) {
  Machine m(MachineType::Plus3);
  loadFourRomBanks(m);

  m.ioWrite(0x1FFD, 0x04, 0);
  CHECK_EQ(m.romBank(), 2);
  CHECK_EQ(m.memRead(0x0000, 0), 0x20);

  m.ioWrite(0x7FFD, 0x10, 0);
  CHECK_EQ(m.romBank(), 3);
  CHECK_EQ(m.memRead(0x0000, 0), 0x30);

  m.ioWrite(0x1FFD, 0x00, 0);
  CHECK_EQ(m.romBank(), 1);
}

TEST(k128_plus3_special_paging_maps_all_four_ram_banks) {
  Machine m(MachineType::Plus3);

  for (int bank = 0; bank < 4; ++bank) {
    m.ioWrite(0x7FFD, u8(bank), 0);
    m.memWrite(0xC000, u8(0x80 + bank), 0);
  }

  m.ioWrite(0x1FFD, 0x01, 0); // CP/M mode
  CHECK(m.specialPaging());

  // The whole 64K is RAM banks 0-3 now, including 0x0000 which was ROM.
  CHECK_EQ(m.memRead(0xC000, 0), 0x83);
  m.memWrite(0x0000, 0xAA, 0);
  CHECK_EQ(m.memRead(0x0000, 0), 0xAA);

  m.ioWrite(0x1FFD, 0x00, 0);
  CHECK(!m.specialPaging());
  CHECK_EQ(m.memRead(0x0000, 0), 0x00); // ROM again
}

// The +2A/+3 CP/M mode is not a single fixed map: bits 1 and 2 of 0x1FFD
// select one of four, and only bank 2/6 at 0x8000 is common to all of them.
TEST(k128_plus3_cpm_paging_selects_four_fixed_maps) {
  Machine m(MachineType::Plus3);

  for (int bank = 0; bank < 8; ++bank) {
    m.ioWrite(0x7FFD, u8(bank), 0);
    m.memWrite(0xC000, u8(0x10 + bank), 0);
  }

  static constexpr u8 Maps[4][4] = {
      {0, 1, 2, 3},
      {4, 5, 6, 7},
      {4, 5, 6, 3},
      {4, 7, 6, 3},
  };
  static constexpr u16 Slot[4] = {0x0000, 0x4000, 0x8000, 0xC000};

  for (int config = 0; config < 4; ++config) {
    m.ioWrite(0x1FFD, u8(0x01 | (config << 1)), 0);
    CHECK(m.specialPaging());
    for (int slot = 0; slot < 4; ++slot) {
      CHECK_EQ(m.memRead(Slot[slot], 0), 0x10 + Maps[config][slot]);
    }
  }
}

// The +2A/+3 only answers 0x7FFD when bit 14 is set, while the 128K/+2
// answers whenever bits 1 and 15 are clear. Port 0x00FD tells them apart.
TEST(k128_plus3_decodes_paging_ports_tighter_than_the_128k) {
  Machine plus3(MachineType::Plus3);
  plus3.ioWrite(0x00FD, 0x07, 0);
  CHECK_EQ(plus3.ramBank(), 0);

  Machine k128(MachineType::Spectrum128);
  k128.ioWrite(0x00FD, 0x07, 0);
  CHECK_EQ(k128.ramBank(), 7);
}

// Only bit 5 of 0x7FFD locks paging, and it freezes 0x1FFD as well.
TEST(k128_plus3_lock_from_7ffd_freezes_both_ports) {
  Machine m(MachineType::Plus3);
  m.ioWrite(0x7FFD, 0x20, 0);
  CHECK(m.pagingLocked());

  m.ioWrite(0x7FFD, 0x07, 0);
  CHECK_EQ(m.ramBank(), 0);
  m.ioWrite(0x1FFD, 0x04, 0);
  CHECK_EQ(m.romBank(), 0);
  m.ioWrite(0x1FFD, 0x01, 0);
  CHECK(!m.specialPaging());
}

// Bit 1 of 0x1FFD is ignored outside CP/M mode: it is neither a lock nor a
// ROM selector, so normal paging keeps working after it is set.
TEST(k128_plus3_1ffd_bit_one_is_not_a_lock) {
  Machine m(MachineType::Plus3);
  m.ioWrite(0x1FFD, 0x02, 0);
  CHECK(!m.pagingLocked());
  CHECK(!m.specialPaging());

  m.ioWrite(0x7FFD, 0x05, 0);
  CHECK_EQ(m.ramBank(), 5);
}

TEST(k128_contention_follows_the_paged_bank) {
  Machine m128(MachineType::Spectrum128);
  const u32 t = visibleTstate(m128);

  m128.ioWrite(0x7FFD, 0x00, 0); // even bank at 0xC000
  CHECK_EQ(m128.contention(0xC000, t), 0);
  m128.ioWrite(0x7FFD, 0x01, 0); // odd bank at 0xC000
  CHECK(m128.contention(0xC000, t) > 0);

  // Bank 5 at 0x4000 is always contended.
  CHECK(m128.contention(0x4000, t) > 0);
  CHECK_EQ(m128.contention(0x8000, t), 0);

  // The +2A/+3 only contends in the 0x4000 window.
  Machine m3(MachineType::Plus3);
  const u32 t3 = visibleTstate(m3);
  m3.ioWrite(0x7FFD, 0x01, 0);
  CHECK_EQ(m3.contention(0xC000, t3), 0);
  CHECK(m3.contention(0x4000, t3) > 0);
}

TEST(k128_pentagon_has_no_contention) {
  Machine m(MachineType::Pentagon);
  const u32 t = visibleTstate(m);
  CHECK_EQ(m.contention(0x4000, t), 0);
  CHECK_EQ(m.contention(0x8000, t), 0);
  CHECK_EQ(m.contention(0xC000, t), 0);
  CHECK_EQ(tstatesPerFrame(m.timings()), 71680u);
  CHECK_EQ(tstatesPerLine(m.timings()), 224u);
}

TEST(k128_frame_is_70908_tstates) {
  Machine m(MachineType::Spectrum128);
  CHECK_EQ(tstatesPerLine(m.timings()), 228u);
  CHECK_EQ(tstatesPerFrame(m.timings()), 70908u);
}

TEST(k128_rom_image_loads_every_16k_bank) {
  Machine m(MachineType::Spectrum128);

  u8 image[0x8000];
  for (size_t i = 0; i < sizeof image; ++i) image[i] = u8(i / 0x4000 == 0 ? 0xA0 : 0xB0);
  CHECK(m.loadRomData(image, sizeof image, 0));
  CHECK(m.loadRomData(image + 0x4000, 0x4000, 1));

  CHECK_EQ(m.memRead(0x0000, 0), 0xA0);
  m.ioWrite(0x7FFD, 0x10, 0);
  CHECK_EQ(m.memRead(0x0000, 0), 0xB0);
}

TEST(k128_machine_types_report_their_timings) {
  CHECK_EQ(rainbow::timingsFor(MachineType::Spectrum48).cpuHz, 3500000u);
  CHECK_EQ(rainbow::timingsFor(MachineType::Spectrum128).cpuHz, 3546900u);
  CHECK_EQ(rainbow::timingsFor(MachineType::Plus3).cpuHz, 3546900u);
  CHECK_EQ(rainbow::timingsFor(MachineType::Pentagon).cpuHz, 3584000u);
  CHECK_EQ(rainbow::timingsFor(MachineType::Spectrum128).ayHz, 1773400u);
}
