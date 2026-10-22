#include "test_framework.h"

#include "core/dac.h"
#include "core/machine.h"
#include "core/turbosound.h"

using rainbow::Ay38912;
using rainbow::Machine;
using rainbow::MachineType;
using rainbow::SpecDrum;
using rainbow::TurboSound;
using rainbow::u16;
using rainbow::u8;

namespace {

// A steady level on channel A: with the tone and noise generators switched off
// the channel simply outputs its volume, which makes the mixer easy to read.
void levelOnA(Ay38912& ay, u8 volume) {
  ay.writeRegister(7, 0xFF);
  ay.writeRegister(8, volume);
  ay.writeRegister(9, 0);
  ay.writeRegister(10, 0);
}

// A short tone on channel A so the output alternates while the chip ticks.
void toneOnA(Ay38912& ay) {
  ay.writeRegister(7, 0x38);
  ay.writeRegister(8, 15);
  ay.writeRegister(0, 1);
  ay.writeRegister(1, 0);
}

// The byte the port takes to select a chip with both outputs enabled.
u8 chipSelect(int chip) {
  const u8 bits = chip == 1 ? 0x02 : (chip == 2 ? 0x01 : 0x03);
  return u8(0x80 | 0x60 | 0x1C | bits);
}

} // namespace

TEST(turbosound_selects_a_chip_from_the_top_bits) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);

  ts.select(chipSelect(0));
  CHECK_EQ(ts.selectedChip(), 0);
  ts.select(chipSelect(1));
  CHECK_EQ(ts.selectedChip(), 1);
  ts.select(chipSelect(2));
  CHECK_EQ(ts.selectedChip(), 2);

  // 00 is what the port reads after a reset, and the hardware treats it as the
  // first chip rather than as "none".
  ts.select(0x80 | 0x60 | 0x1C);
  CHECK_EQ(ts.selectedChip(), 0);
}

TEST(turbosound_keeps_a_register_index_per_chip) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);

  ts.select(chipSelect(1));
  ts.select(8); // register 8 of the second chip
  ts.write(15);
  CHECK_EQ(ts.chip(1).volume(0), 15);
  CHECK_EQ(ts.chip(0).volume(0), 0);

  // The first chip still has its own index, so switching back keeps working.
  ts.select(chipSelect(0));
  ts.select(8);
  CHECK_EQ(ts.chip(0).selected(), 8);
  ts.write(3);
  CHECK_EQ(ts.chip(0).volume(0), 3);
  CHECK_EQ(ts.chip(1).volume(0), 15);
}

TEST(turbosound_ignores_the_top_bit_when_chip_select_is_off) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(false);

  ts.select(0xFE); // a plain register number on the 128 and the +3
  CHECK_EQ(ts.selectedChip(), 0);
  CHECK_EQ(ts.chip(0).selected(), 0x0E);
}

TEST(turbosound_stereo_bits_mute_one_side_of_a_chip) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);
  ts.setStereoMode(Ay38912::Stereo::ABC);

  // Channel B sits in the middle under ABC, so it reaches both sides.
  ts.chip(0).writeRegister(7, 0xFF);
  ts.chip(0).writeRegister(9, 15);
  CHECK(ts.sampleLeft() > 0.3f);
  CHECK(ts.sampleRight() > 0.3f);

  // Bit 6 off, bit 5 on: the left output of the first chip goes quiet.
  ts.select(0x80 | 0x20 | 0x1C | 0x03);
  CHECK_EQ(ts.sampleLeft(), 0.0f);
  CHECK(ts.sampleRight() > 0.3f);
  CHECK(!ts.leftEnabled(0));
  CHECK(ts.rightEnabled(0));

  // Bit 6 on, bit 5 off is the other way round.
  ts.select(0x80 | 0x40 | 0x1C | 0x03);
  CHECK(ts.sampleLeft() > 0.3f);
  CHECK_EQ(ts.sampleRight(), 0.0f);
}

TEST(turbosound_adds_the_chips_together) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);
  ts.setStereoMode(Ay38912::Stereo::ABC);
  levelOnA(ts.chip(0), 15);

  const float one = ts.sampleLeft();
  levelOnA(ts.chip(1), 15);
  const float two = ts.sampleLeft();
  CHECK(two > one);

  levelOnA(ts.chip(2), 15);
  CHECK(ts.sampleLeft() > two);
}

TEST(turbosound_freezes_the_extra_chips_until_enabled) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);
  ts.setStereoMode(Ay38912::Stereo::Mono);
  toneOnA(ts.chip(1));

  for (int i = 0; i < 100; ++i) {
    ts.tick(10);
    CHECK_EQ(ts.chip(1).channel(0), 0.0f);
  }

  ts.setEnabled(true);
  bool heard = false;
  for (int i = 0; i < 100; ++i) {
    ts.tick(10);
    if (ts.chip(1).channel(0) > 0.0f) heard = true;
  }
  CHECK(heard);
}

TEST(turbosound_stereo_arrangement_moves_the_middle_channel) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);
  ts.chip(0).writeRegister(7, 0xFF);
  ts.chip(0).writeRegister(8, 0);
  ts.chip(0).writeRegister(9, 15); // B only
  ts.chip(0).writeRegister(10, 0);

  // ABC puts B in the middle, so it is heard on both sides.
  ts.setStereoMode(Ay38912::Stereo::ABC);
  CHECK(ts.sampleLeft() > 0.0f);
  CHECK(ts.sampleRight() > 0.0f);

  // ACB moves B to the right and C to the middle, so B leaves the left alone.
  ts.setStereoMode(Ay38912::Stereo::ACB);
  CHECK_EQ(ts.sampleLeft(), 0.0f);
  CHECK(ts.sampleRight() > 0.5f);
}

TEST(turbosound_mono_bit_sends_a_chip_to_both_sides) {
  TurboSound ts;
  ts.reset();
  ts.setChipSelectEnabled(true);
  ts.setStereoMode(Ay38912::Stereo::ABC);
  levelOnA(ts.chip(0), 15);

  CHECK(ts.sampleLeft() > 0.0f);
  CHECK_EQ(ts.sampleRight(), 0.0f);

  ts.setChipMono(0, true);
  CHECK(ts.sampleLeft() > 0.0f);
  CHECK_EQ(ts.sampleLeft(), ts.sampleRight());
}

TEST(machine_next_selects_a_chip_through_the_ay_port) {
  Machine m(MachineType::Next);

  m.ioWrite(0xFFFD, chipSelect(1), 0);
  CHECK_EQ(m.sound().selectedChip(), 1);
  m.ioWrite(0xFFFD, 8, 0);
  m.ioWrite(0xBFFD, 15, 0);
  CHECK_EQ(m.sound().chip(1).volume(0), 15);
  CHECK_EQ(m.sound().chip(0).volume(0), 0);

  // The data port follows the selection back to the first chip.
  m.ioWrite(0xFFFD, chipSelect(0), 0);
  m.ioWrite(0xFFFD, 8, 0);
  CHECK_EQ(m.ioRead(0xBFFD, 0), 0);
}

TEST(machine_128k_reads_the_top_bit_as_a_register_number) {
  Machine m(MachineType::Spectrum128);

  m.ioWrite(0xFFFD, 0xFE, 0);
  CHECK_EQ(m.sound().selectedChip(), 0);
  CHECK_EQ(m.ay().selected(), 0x0E);
}

TEST(machine_next_nextreg_08_turns_turbosound_on_and_picks_the_arrangement) {
  Machine m(MachineType::Next);
  CHECK(!m.sound().enabled());
  CHECK(m.sound().stereoMode() == Ay38912::Stereo::ABC);

  m.writeNextReg(0x08, 0x22); // TurboSound on, ACB
  CHECK(m.sound().enabled());
  CHECK(m.sound().stereoMode() == Ay38912::Stereo::ACB);

  m.writeNextReg(0x08, 0x00);
  CHECK(!m.sound().enabled());
  CHECK(m.sound().stereoMode() == Ay38912::Stereo::ABC);
}

TEST(machine_next_nextreg_09_sets_the_mono_bit_of_one_chip) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x09, 0x80);
  CHECK(!m.sound().chipMono(0));
  CHECK(!m.sound().chipMono(1));
  CHECK(m.sound().chipMono(2));

  m.writeNextReg(0x09, 0x20);
  CHECK(m.sound().chipMono(0));
  CHECK(!m.sound().chipMono(2));
}

TEST(dac_starts_silent_and_the_ports_are_recognised) {
  SpecDrum dac;
  dac.reset();
  CHECK_EQ(dac.dac(0), 0x80);
  CHECK_EQ(dac.sampleLeft(), 0.0f);
  CHECK_EQ(dac.sampleRight(), 0.0f);

  for (u16 port : {0x000F, 0x001F, 0x003F, 0x004F, 0x005F, 0x00B3, 0x00DF,
                   0x00F1, 0x00F3, 0x00F9, 0x00FB}) {
    CHECK(SpecDrum::handles(port));
  }
  CHECK(!SpecDrum::handles(0x00FE));
  CHECK(!SpecDrum::handles(0x001E));
}

TEST(dac_port_df_writes_the_two_centre_channels) {
  Machine m(MachineType::Next);
  m.ioWrite(0xDF, 0xFF, 0);
  CHECK_EQ(m.dacs().dac(0), 0xFF);
  CHECK_EQ(m.dacs().dac(3), 0xFF);
  CHECK_EQ(m.dacs().dac(1), 0x80);
  CHECK_EQ(m.dacs().dac(2), 0x80);

  // A and D sit on opposite sides, so 0xDF is the centre channel.
  CHECK(m.dacs().sampleLeft() > 0.45f);
  CHECK_EQ(m.dacs().sampleLeft(), m.dacs().sampleRight());

  m.ioWrite(0xDF, 0x00, 0);
  CHECK(m.dacs().sampleLeft() < -0.45f);
}

TEST(dac_left_and_right_ports_pan_a_single_channel) {
  Machine m(MachineType::Next);

  m.ioWrite(0x1F, 0xFF, 0); // DAC A, left
  CHECK(m.dacs().sampleLeft() > 0.45f);
  CHECK_EQ(m.dacs().sampleRight(), 0.0f);

  m.ioWrite(0x1F, 0x80, 0);
  m.ioWrite(0x4F, 0xFF, 0); // DAC C, right
  CHECK_EQ(m.dacs().sampleLeft(), 0.0f);
  CHECK(m.dacs().sampleRight() > 0.45f);

  m.ioWrite(0x0F, 0xFF, 0); // DAC B joins A on the left
  CHECK(m.dacs().sampleLeft() > 0.45f);
}

TEST(dac_nextreg_mirrors_write_and_read_back) {
  Machine m(MachineType::Next);

  m.writeNextReg(0x2C, 0x00); // DAC B
  CHECK_EQ(m.dacs().dac(1), 0x00);
  CHECK_EQ(m.readNextReg(0x2C), 0x00);

  m.writeNextReg(0x2D, 0xFF); // DACs A and D
  CHECK_EQ(m.dacs().dac(0), 0xFF);
  CHECK_EQ(m.dacs().dac(3), 0xFF);
  CHECK_EQ(m.readNextReg(0x2D), 0xFF);

  m.writeNextReg(0x2E, 0x00); // DAC C
  CHECK_EQ(m.dacs().dac(2), 0x00);
  CHECK_EQ(m.readNextReg(0x2E), 0x00);
}

TEST(dac_is_ignored_on_the_older_machines) {
  Machine m(MachineType::Spectrum128);
  m.ioWrite(0xDF, 0xFF, 0);
  CHECK_EQ(m.dacs().dac(0), 0x80);
  CHECK_EQ(m.dacs().dac(3), 0x80);
}
