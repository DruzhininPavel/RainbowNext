#include "test_framework.h"

#include "core/machine.h"

using mynext::Machine;
using mynext::MachineType;
using mynext::u32;
using mynext::u8;
using mynext::tstatesPerLine;
using mynext::tstatesPerFrame;

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

TEST(k128_plus3_paging_lock_covers_both_ports) {
  Machine m(MachineType::Plus3);
  m.ioWrite(0x1FFD, 0x02, 0); // lock
  CHECK(m.pagingLocked());

  m.ioWrite(0x7FFD, 0x07, 0);
  CHECK_EQ(m.ramBank(), 0);
  m.ioWrite(0x1FFD, 0x04, 0);
  CHECK_EQ(m.romBank(), 0);
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
  CHECK_EQ(mynext::timingsFor(MachineType::Spectrum48).cpuHz, 3500000u);
  CHECK_EQ(mynext::timingsFor(MachineType::Spectrum128).cpuHz, 3546900u);
  CHECK_EQ(mynext::timingsFor(MachineType::Plus3).cpuHz, 3546900u);
  CHECK_EQ(mynext::timingsFor(MachineType::Pentagon).cpuHz, 3584000u);
  CHECK_EQ(mynext::timingsFor(MachineType::Spectrum128).ayHz, 1773400u);
}
