#include "test_framework.h"

#include "core/ay.h"
#include "core/machine.h"

using rainbow::Ay38912;
using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u8;

namespace {

// Tone A only, no noise: R7 bit 0 enables tone A, bits 3-5 disable the noise
// generators. Volume 15 is the loudest step.
void setupTone(Ay38912& ay, unsigned period) {
  ay.reset();
  ay.writeRegister(7, 0x38);
  ay.writeRegister(8, 15);
  ay.writeRegister(0, u8(period & 0xFF));
  ay.writeRegister(1, u8((period >> 8) & 0x0F));
}

} // namespace

TEST(ay_registers_round_trip) {
  Ay38912 ay;
  ay.reset();

  ay.select(7);
  CHECK_EQ(ay.selected(), 7);
  ay.write(0x38);
  CHECK_EQ(ay.read(), 0x38);
  CHECK_EQ(ay.readRegister(7), 0x38);

  ay.select(0x1F); // the register index is four bits wide
  CHECK_EQ(ay.selected(), 0x0F);
}

TEST(ay_tone_period_sets_the_toggle_rate) {
  Ay38912 ay;
  setupTone(ay, 50);
  CHECK_EQ(ay.tonePeriod(0), 50);

  // The chip runs at half the CPU clock, so 2 CPU cycles are one chip tick.
  // A period of 50 toggles the output every 50 ticks.
  int toggles = 0;
  bool last = ay.channel(0) > 0.5f;
  for (int i = 0; i < 2000; ++i) {
    ay.tick(2);
    const bool now = ay.channel(0) > 0.5f;
    if (now != last) ++toggles;
    last = now;
  }
  CHECK_EQ(toggles, 40);

  // Half the period gives twice the toggles over the same window.
  setupTone(ay, 25);
  toggles = 0;
  last = ay.channel(0) > 0.5f;
  for (int i = 0; i < 2000; ++i) {
    ay.tick(2);
    const bool now = ay.channel(0) > 0.5f;
    if (now != last) ++toggles;
    last = now;
  }
  CHECK_EQ(toggles, 80);
}

TEST(ay_volume_register_scales_the_output) {
  Ay38912 ay;
  setupTone(ay, 100);
  ay.writeRegister(8, 15);
  ay.tick(2);
  const float loud = ay.channel(0);
  CHECK(loud > 0.99f);

  setupTone(ay, 100);
  ay.writeRegister(8, 8);
  ay.tick(2);
  const float quiet = ay.channel(0);
  CHECK(quiet > 0.0f);
  CHECK(quiet < loud * 0.5f);

  setupTone(ay, 100);
  ay.writeRegister(8, 0);
  ay.tick(2);
  CHECK_EQ(ay.channel(0), 0);
}

TEST(ay_noise_is_deterministic_and_never_sticks) {
  Ay38912 first;
  Ay38912 second;
  for (Ay38912* ay : {&first, &second}) {
    ay->reset();
    ay->writeRegister(7, 0x07); // all tones off, noise on
    ay->writeRegister(8, 15);
    ay->writeRegister(6, 1);
  }

  int transitions = 0;
  bool last = first.channel(0) > 0.0f;
  for (int i = 0; i < 500; ++i) {
    first.tick(2);
    second.tick(2);
    const bool now = first.channel(0) > 0.0f;
    if (now != last) ++transitions;
    last = now;
    CHECK_EQ(first.channel(0) > 0.0f, second.channel(0) > 0.0f);
  }
  CHECK(transitions > 100);
}

TEST(ay_envelope_shape_zero_decays_and_holds) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(11, 1);
  ay.writeRegister(12, 0);
  ay.writeRegister(13, 0x00); // decay, no continue

  CHECK_EQ(ay.envelopePosition(), 15);
  CHECK(ay.envelopeRunning());

  // One envelope step lasts 16 * period chip ticks.
  ay.tick(2 * 16);
  CHECK_EQ(ay.envelopePosition(), 14);

  ay.tick(2 * 16 * 15);
  CHECK_EQ(ay.envelopePosition(), 0);
  CHECK(!ay.envelopeRunning());

  ay.tick(2 * 16 * 64);
  CHECK_EQ(ay.envelopePosition(), 0);
}

TEST(ay_envelope_shape_four_attacks_and_holds) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(11, 1);
  ay.writeRegister(13, 0x04); // attack, no continue

  CHECK_EQ(ay.envelopePosition(), 0);
  ay.tick(2 * 16 * 16);
  CHECK_EQ(ay.envelopePosition(), 15);
  CHECK(!ay.envelopeRunning());
}

TEST(ay_envelope_shape_eight_repeats) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(11, 1);
  ay.writeRegister(13, 0x08); // decay, continue

  ay.tick(2 * 16 * 16);
  CHECK_EQ(ay.envelopePosition(), 15); // wrapped back to the top
  CHECK(ay.envelopeRunning());
}

TEST(ay_envelope_drives_the_channel_volume) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(7, 0x38); // tone on, noise off
  ay.writeRegister(8, 0x10); // channel A follows the envelope
  ay.writeRegister(11, 1);
  ay.writeRegister(13, 0x00);

  ay.tick(2 * 16 * 15);
  CHECK_EQ(ay.envelopePosition(), 0);
  // The tone output is gated by the envelope, which is at zero now.
  CHECK_EQ(ay.channel(0), 0.0f);

  ay.writeRegister(13, 0x04); // restart as an attack
  ay.tick(2 * 16 * 15);
  CHECK_EQ(ay.envelopePosition(), 15);
  // At the top of the envelope the tone is loud whenever it is high.
  bool loud = false;
  for (int i = 0; i < 200 && !loud; ++i) {
    ay.tick(2);
    loud = ay.channel(0) > 0.9f;
  }
  CHECK(loud);
}

TEST(ay_mixer_register_disables_channels) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(8, 15);
  ay.writeRegister(0, 10);

  // Tone A disabled: the channel sits at its volume instead of toggling.
  ay.writeRegister(7, 0x3F);
  const float steady = ay.channel(0);
  CHECK(steady > 0.99f);
  for (int i = 0; i < 100; ++i) {
    ay.tick(2);
    CHECK_EQ(ay.channel(0) > 0.99f, 1);
  }

  // Both tone and noise enabled again: the channel toggles.
  ay.writeRegister(7, 0x38);
  bool sawLow = false;
  bool sawHigh = false;
  for (int i = 0; i < 100; ++i) {
    ay.tick(2);
    if (ay.channel(0) > 0.5f) sawHigh = true; else sawLow = true;
  }
  CHECK(sawHigh);
  CHECK(sawLow);
}

TEST(ay_stereo_places_channels_left_and_right) {
  Ay38912 ay;
  ay.reset();
  ay.writeRegister(7, 0x3F); // all tone and noise off: steady levels
  ay.writeRegister(8, 15);   // A
  ay.writeRegister(9, 0);    // B
  ay.writeRegister(10, 0);   // C

  ay.setStereo(Ay38912::Stereo::ABC);
  CHECK(ay.sampleLeft() > 0.5f);
  CHECK_EQ(ay.sampleRight(), 0.0f);

  // ACB keeps A on the left and puts B on the right instead of C.
  ay.setStereo(Ay38912::Stereo::ACB);
  CHECK(ay.sampleLeft() > 0.5f);
  CHECK_EQ(ay.sampleRight(), 0.0f);

  ay.writeRegister(8, 0);
  ay.writeRegister(9, 15);
  CHECK_EQ(ay.sampleLeft(), 0.0f);
  CHECK(ay.sampleRight() > 0.5f);

  // Under ABC the middle channel B feeds both speakers.
  ay.setStereo(Ay38912::Stereo::ABC);
  CHECK(ay.sampleLeft() > 0.0f);
  CHECK(ay.sampleRight() > 0.0f);

  // C is audible on the right only.
  ay.writeRegister(9, 0);
  ay.writeRegister(10, 15);
  CHECK_EQ(ay.sampleLeft(), 0.0f);
  CHECK(ay.sampleRight() > 0.5f);

  ay.setStereo(Ay38912::Stereo::Mono);
  CHECK(ay.sampleLeft() > 0.0f);
  CHECK_EQ(ay.sampleLeft(), ay.sampleRight());
}

TEST(machine_ay_ports_answer_on_128k_only) {
  Machine m128(MachineType::Spectrum128);
  m128.ioWrite(0xFFFD, 7, 0);
  m128.ioWrite(0xBFFD, 0x38, 0);
  m128.ioWrite(0xFFFD, 7, 0);
  CHECK_EQ(m128.ioRead(0xBFFD, 0), 0x38);

  // The AY runs while the machine ticks, so a tone becomes audible.
  m128.ioWrite(0xFFFD, 0, 0);
  m128.ioWrite(0xBFFD, 50, 0);
  m128.ioWrite(0xFFFD, 8, 0);
  m128.ioWrite(0xBFFD, 15, 0);
  CHECK_EQ(m128.ay().tonePeriod(0), 50);

  Machine m48(MachineType::Spectrum48);
  m48.ioWrite(0xFFFD, 7, 0);
  m48.ioWrite(0xBFFD, 0x38, 0);
  m48.ioWrite(0xFFFD, 7, 0);
  CHECK_EQ(m48.ioRead(0xBFFD, 0), 0xFF); // no chip on the 48K
  CHECK(!m48.hasAy());
}
