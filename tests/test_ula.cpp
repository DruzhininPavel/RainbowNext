#include "test_framework.h"

#include "core/keyboard.h"
#include "core/machine.h"
#include "core/ula.h"

using rainbow::Keyboard;
using rainbow::Machine;
using rainbow::MachineType;
using rainbow::Ula;

TEST(keyboard_matrix) {
  Keyboard kb;
  kb.reset();
  CHECK_EQ(kb.read(0xFE), 0xFF);

  kb.setKey(0, 0, true); // Caps Shift
  CHECK_EQ(kb.read(0xFE), 0xFE);
  CHECK_EQ(kb.read(0xFD), 0xFF);

  kb.setKey(1, 1, true); // S
  CHECK_EQ(kb.read(0xFE), 0xFE);
  CHECK_EQ(kb.read(0xFD), 0xFD);

  kb.setKey(0, 0, false);
  kb.setKey(1, 1, false);
  CHECK_EQ(kb.read(0xFE), 0xFF);
}

TEST(ula_port_fe) {
  Ula ula;
  Keyboard kb;
  ula.reset(false);

  CHECK_EQ(ula.read(0xFE, kb), 0xBF);

  ula.write(0xFE, 0x02);
  CHECK_EQ(ula.border(), 2);
  CHECK_EQ(ula.beeper(), 0);

  ula.write(0xFE, 0x10);
  CHECK_EQ(ula.border(), 0);
  CHECK_EQ(ula.beeper(), 1);

  kb.setKey(0, 0, true);
  CHECK_EQ(ula.read(0xFE, kb), 0xBE);
}

TEST(machine_timings_48k) {
  Machine m(MachineType::Spectrum48);
  CHECK_EQ(m.tstatesPerLineValue(), 224);
  CHECK_EQ(m.tstatesPerFrameValue(), 69888);
  CHECK_EQ(m.timings().cpuHz, 3500000);
}

TEST(machine_runs_exactly_one_frame) {
  Machine m(MachineType::Spectrum48);
  m.runFrame();
  CHECK_EQ(m.frames(), 1);
  CHECK_EQ(m.cpu().cycles(), 69888);
  CHECK_EQ(m.now(), 0);
}

TEST(machine_contention_window) {
  Machine m(MachineType::Spectrum48);
  const rainbow::u32 insideScreen = 48 * 224 + 24;
  CHECK(m.contention(0x4000, insideScreen) > 0);
  CHECK_EQ(m.contention(0x4000, 0), 0);        // above the screen
  CHECK_EQ(m.contention(0x8000, insideScreen), 0); // not the screen bank on 48K
  CHECK_EQ(m.contention(0xC000, insideScreen), 0);
}

TEST(machine_floating_bus) {
  Machine m(MachineType::Spectrum48);
  m.memWrite(0x5800, 0xCD, 0); // attribute of column 0, row 0
  m.memWrite(0x4000, 0xAB, 0); // pixel byte of column 0

  m.tick(48 * 224 + 24 + 3); // inside the display, attribute phase
  CHECK_EQ(m.ioRead(0x00FC, 0), 0xCD);

  m.tick(5); // pixel phase, column 2
  CHECK_EQ(m.ioRead(0x00FC, 0), 0x00);
}

TEST(machine_paging_128k) {
  Machine m(MachineType::Spectrum128);

  m.ioWrite(0x7FFD, 0x03, 0);
  CHECK_EQ(m.ramBank(), 3);
  CHECK_EQ(m.screenBank(), 5);
  CHECK_EQ(m.romBank(), 0);

  m.ioWrite(0x7FFD, 0x18, 0);
  CHECK_EQ(m.screenBank(), 7);
  CHECK_EQ(m.romBank(), 1);

  m.ioWrite(0x7FFD, 0x20, 0);
  m.ioWrite(0x7FFD, 0x05, 0); // locked now
  CHECK_EQ(m.ramBank(), 0);
}

TEST(machine_rom_mapping) {
  Machine m(MachineType::Spectrum48);
  const rainbow::u8 rom[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  CHECK(m.loadRomData(rom, sizeof rom, 0));
  CHECK(m.romLoaded());
  CHECK_EQ(m.memRead(0x0000, 0), 0xDE);
  CHECK_EQ(m.memRead(0x0003, 0), 0xEF);
  CHECK_EQ(m.memRead(0x4000, 0), 0x00);
}

TEST(machine_audio_rate) {
  Machine m(MachineType::Spectrum48);
  m.setSampleRate(44100);
  m.runFrame();
  // The buffer is interleaved stereo, so a 50.08 Hz frame at 44.1 kHz is
  // ~880 frames of two samples each.
  const size_t samples = m.audioBuffer().size() / 2;
  CHECK(samples > 820);
  CHECK(samples < 940);
  for (float s : m.audioBuffer()) CHECK(s >= -0.5f && s <= 0.5f);
}

TEST(machine_border_rendering) {
  Machine m(MachineType::Spectrum48);
  m.ioWrite(0x00FE, 0x02, 0); // red border
  m.runFrame();
  // Top-left pixel of the framebuffer is border.
  CHECK_EQ(m.framebuffer()[0], Machine::Palette[2]);
  CHECK_EQ(m.framebuffer()[Machine::FrameWidth * Machine::FrameHeight - 1],
           Machine::Palette[2]);
}

TEST(keyboard_pad_layer_is_separate_from_the_matrix) {
  rainbow::Keyboard kb;
  kb.setKey(7, 0, true); // SPACE held on the host keyboard
  kb.setPadKey(7, 0, true);
  kb.setPadKey(7, 0, false);
  // Letting go of the pad must not clear the key the keyboard still holds.
  CHECK(kb.isDown(7, 0));
  CHECK_EQ(kb.read(0x7F), 0xFE);

  kb.setKey(7, 0, false);
  CHECK(!kb.isDown(7, 0));
  CHECK_EQ(kb.read(0x7F), 0xFF);
}

TEST(keyboard_pad_layer_reads_like_a_pressed_key) {
  rainbow::Keyboard kb;
  kb.setPadKey(6, 0, true); // ENTER
  // Row 6 is selected by pulling A14 low.
  CHECK_EQ(kb.read(0xBF), 0xFE);
  // A row that is not selected still reads as untouched.
  CHECK_EQ(kb.read(0x7F), 0xFF);

  kb.reset();
  CHECK(!kb.isDown(6, 0));
  CHECK_EQ(kb.read(0xBF), 0xFF);
}
