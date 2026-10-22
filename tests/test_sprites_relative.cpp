// Relative and unified (big) sprites, plus the sprite status port 0x303B.
//
// A relative sprite hangs off the last anchor that precedes it in the list: it
// borrows the anchor's position, pattern size and optionally its pattern and
// palette offset. An anchor whose byte 4 has bit 5 set turns its relatives into
// one big sprite that follows the anchor's mirror, rotate and scale.

#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "core/sprites.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::Sprites;
using rainbow::u32;
using rainbow::u8;

namespace {

void renderOneFrame(Machine& m) { m.runCycles(m.tstatesPerFrameValue()); }

u32 surfacePixel(const Machine& m, int x, int y) {
  return m.framebuffer()[size_t(y) * size_t(Machine::FrameWidth) + size_t(x)];
}

void writePalette8(Machine& m, u8 index, u8 value) {
  m.writeNextReg(0x40, index);
  m.writeNextReg(0x41, value);
}

void selectSpritePalette(Machine& m) {
  m.writeNextReg(0x43, 0x20); // bits 6:4 = 010 -> sprites first palette
  writePalette8(m, 0x01, 0x1C); // bright green
  writePalette8(m, 0x02, 0xE3); // bright magenta
  m.writeNextReg(0x43, 0x00);
}

// Pattern 0: colour 1 everywhere, colour 2 on the top row, transparent below.
void writeFullPattern(Machine& m) {
  m.ioWrite(0x303B, 0, 0);
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      u8 value = 0x01;
      if (row == 0) value = 0x02;
      if (row == 15) value = 0xE3;
      m.ioWrite(0x005B, value, 0);
    }
  }
}

// An anchor sprite: extended, four attribute bytes plus byte 4.
void writeAnchor(Machine& m, int sprite, int x, int y, u8 byte2, u8 pattern6,
                 u8 byte4, bool visible = true) {
  m.writeNextReg(0x34, u8(sprite));
  m.writeNextReg(0x35, u8(x & 0xFF));
  m.writeNextReg(0x36, u8(y & 0xFF));
  m.writeNextReg(0x37, u8(byte2));
  m.writeNextReg(0x38, u8((visible ? 0x80 : 0x00) | 0x40 | (pattern6 & 0x3F)));
  m.writeNextReg(0x39, u8(byte4));
}

// A relative sprite: byte 4 is 0 1 N6 XX YY PO, so bit 6 is always set.
void writeRelative(Machine& m, int sprite, int dx, int dy, u8 byte2,
                   u8 pattern6, bool addAnchorPattern,
                   bool visible = true) {
  m.writeNextReg(0x34, u8(sprite));
  m.writeNextReg(0x35, u8(dx & 0xFF));
  m.writeNextReg(0x36, u8(dy & 0xFF));
  m.writeNextReg(0x37, u8(byte2));
  m.writeNextReg(0x38, u8((visible ? 0x80 : 0x00) | 0x40 | (pattern6 & 0x3F)));
  m.writeNextReg(0x39, u8(0x40 | (addAnchorPattern ? 0x01 : 0x00)));
}

void enableSprites(Machine& m, u8 control = 0x01) {
  m.writeNextReg(0x15, control);
}

} // namespace

// ---------------------------------------------------------------------------
// Relative sprites
// ---------------------------------------------------------------------------

TEST(sprite_relative_origin_follows_the_anchor) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 40, 0x00, 0, 0x00);
  writeRelative(m, 1, 16, 8, 0x00, 0, false);

  const auto states = m.sprites().resolve();
  CHECK(states[1].visible);
  CHECK_EQ(states[1].x, 48);
  CHECK_EQ(states[1].y, 48);
}

TEST(sprite_relative_negative_offset_moves_left_and_up) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 64, 64, 0x00, 0, 0x00);
  writeRelative(m, 1, -16, -24, 0x00, 0, false); // 0xF0 / 0xE8

  const auto states = m.sprites().resolve();
  CHECK_EQ(states[1].x, 48);
  CHECK_EQ(states[1].y, 40);
}

TEST(sprite_relative_is_hidden_by_an_invisible_anchor) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00, false);
  writeRelative(m, 1, 16, 0, 0x00, 0, false);

  CHECK(m.sprites().relative(1));
  CHECK(!m.sprites().resolve()[1].visible);
}

TEST(sprite_relative_also_needs_its_own_visible_bit) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);
  writeRelative(m, 1, 16, 0, 0x00, 0, false, false);

  CHECK(!m.sprites().resolve()[1].visible);
}

TEST(sprite_relative_inherits_the_anchor_pattern_size) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x80); // H = 1, so 4-bit patterns
  writeRelative(m, 1, 16, 0, 0x00, 0, false);

  // The relative form has no size bit of its own, so fourBit() is false there
  // and the resolved state takes the anchor's.
  CHECK(!m.sprites().fourBit(1));
  CHECK(m.sprites().resolve()[1].fourBit);
}

TEST(sprite_relative_palette_offset_bit_adds_the_anchor) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x50, 0, 0x00); // anchor palette offset 5
  writeRelative(m, 1, 16, 0, 0x01, 0, false); // PR set
  writeRelative(m, 2, 32, 0, 0x00, 0, false); // PR clear

  const auto states = m.sprites().resolve();
  CHECK_EQ(states[1].paletteOffset, 5);
  CHECK_EQ(states[2].paletteOffset, 0);
}

TEST(sprite_relative_pattern_offset_bit_adds_the_anchor_pattern) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x00, 3, 0x00); // pattern 6
  writeRelative(m, 1, 16, 0, 0x00, 3, true);  // PO set
  writeRelative(m, 2, 32, 0, 0x00, 3, false); // PO clear

  const auto states = m.sprites().resolve();
  CHECK_EQ(states[0].pattern, 6);
  CHECK_EQ(states[1].pattern, 12);
  CHECK_EQ(states[2].pattern, 6);
}

TEST(sprite_composite_relative_keeps_its_own_mirror_and_scale) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 32, 32, 0x08, 0, 0x08); // anchor: mirrorX, scaleX 2
  writeRelative(m, 1, 16, 0, 0x04, 0, false); // composite: its own mirrorY

  const auto states = m.sprites().resolve();
  // A composite relative is an independent sprite; only the position comes
  // from the anchor.
  CHECK_EQ(states[1].x, 48);
  CHECK(!states[1].mirrorX);
  CHECK(states[1].mirrorY);
  CHECK_EQ(states[1].scaleX, 1);
}

// ---------------------------------------------------------------------------
// Unified (big) sprites
// ---------------------------------------------------------------------------

TEST(sprite_unified_copies_the_anchor_mirror) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 64, 64, 0x08, 0, 0x20); // T = 1, mirrorX
  writeRelative(m, 1, 16, 0, 0x00, 0, false);

  const auto states = m.sprites().resolve();
  // The mirror flips the offset around the anchor, not the part's own flags.
  CHECK_EQ(states[1].x, 48);
  CHECK(states[1].mirrorX);
}

TEST(sprite_unified_scales_the_offset_with_the_anchor) {
  Machine m(MachineType::Next);
  // XX = 01 -> scaleX 2, YY = 10 -> scaleY 4.
  writeAnchor(m, 0, 64, 64, 0x00, 0, 0x20 | 0x08 | 0x04);
  writeRelative(m, 1, 16, 8, 0x00, 0, false);

  const auto states = m.sprites().resolve();
  CHECK_EQ(states[0].scaleX, 2);
  CHECK_EQ(states[0].scaleY, 4);
  CHECK_EQ(states[1].x, 96); // 64 + 16 * 2
  CHECK_EQ(states[1].y, 96); // 64 + 8 * 4
  CHECK_EQ(states[1].scaleX, 2);
  CHECK_EQ(states[1].scaleY, 4);
}

TEST(sprite_unified_rotation_turns_the_part_around_the_anchor) {
  Machine m(MachineType::Next);
  writeAnchor(m, 0, 64, 64, 0x02, 0, 0x20); // T = 1, rotate
  writeRelative(m, 1, 16, 0, 0x00, 0, false);

  const auto states = m.sprites().resolve();
  CHECK(states[0].rotate);
  CHECK(states[1].rotate);
  // A quarter clockwise turn maps the +X offset onto +Y.
  CHECK_EQ(states[1].x, 64);
  CHECK_EQ(states[1].y, 80);
}

TEST(sprite_unified_relative_draws_next_to_its_anchor) {
  Machine m(MachineType::Next);
  selectSpritePalette(m);
  enableSprites(m);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x20);
  writeRelative(m, 1, 16, 0, 0x00, 0, false);

  renderOneFrame(m);
  // The part lands one cell to the right of the anchor and paints the same
  // pattern, so its top row carries colour 2.
  CHECK_EQ(surfacePixel(m, 32, 32), m.palette().colour(
                                        rainbow::NextPalette::SpritesFirst, 2));
  CHECK_EQ(surfacePixel(m, 48, 32), m.palette().colour(
                                        rainbow::NextPalette::SpritesFirst, 2));
}

TEST(sprite_anchor_without_byte_4_is_never_relative) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x34, 0);
  m.writeNextReg(0x35, 0x20);
  m.writeNextReg(0x36, 0x20);
  m.writeNextReg(0x37, 0x00);
  m.writeNextReg(0x38, 0x80); // visible, not extended

  CHECK(!m.sprites().relative(0));
  CHECK_EQ(m.sprites().resolve()[0].x, 0x20);
}

// ---------------------------------------------------------------------------
// Status port 0x303B
// ---------------------------------------------------------------------------

TEST(sprite_collision_is_reported_and_clears_on_read) {
  Machine m(MachineType::Next);
  enableSprites(m);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);
  writeAnchor(m, 1, 32, 32, 0x00, 0, 0x00);

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0) & 0x01, 1);
  CHECK_EQ(m.ioRead(0x303B, 0) & 0x01, 0); // the read cleared it
}

TEST(sprite_collision_ignores_transparent_pixels) {
  Machine m(MachineType::Next);
  enableSprites(m);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);
  // The bottom row of every pattern is transparent, so touching there is not
  // an overlap.
  writeAnchor(m, 1, 32, 32 + 15, 0x00, 0, 0x00);

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0) & 0x01, 0);
}

TEST(sprite_collision_is_not_reported_for_a_single_sprite) {
  Machine m(MachineType::Next);
  enableSprites(m);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0), 0);
}

TEST(sprite_overflow_is_reported_when_a_line_runs_out) {
  Machine m(MachineType::Next);
  enableSprites(m);
  // All the sprites share the same sixteen scanlines, so the hundredth is the
  // last one the hardware can prepare.
  for (int sprite = 0; sprite < 101; ++sprite) {
    writeAnchor(m, sprite, 32, 32, 0x00, 0, 0x00);
  }

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0) & 0x02, 0x02);
}

TEST(sprite_overflow_is_clear_when_every_sprite_fits) {
  Machine m(MachineType::Next);
  enableSprites(m);
  for (int sprite = 0; sprite < 100; ++sprite) {
    writeAnchor(m, sprite, 32, 32, 0x00, 0, 0x00);
  }

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0) & 0x02, 0x00);
}

TEST(sprite_status_is_zero_when_sprites_are_disabled) {
  Machine m(MachineType::Next);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);
  writeAnchor(m, 1, 32, 32, 0x00, 0, 0x00);

  renderOneFrame(m);
  CHECK_EQ(m.ioRead(0x303B, 0), 0);
}

TEST(sprite_reset_drops_the_lockstep_and_the_status_flags) {
  Machine m(MachineType::Next);
  m.writeNextReg(0x09, 0x10);
  m.writeNextReg(0x34, 5);
  enableSprites(m);
  writeFullPattern(m);
  writeAnchor(m, 0, 32, 32, 0x00, 0, 0x00);
  writeAnchor(m, 1, 32, 32, 0x00, 0, 0x00);
  renderOneFrame(m);

  m.reset();
  CHECK(!m.sprites().lockstep());
  CHECK_EQ(m.ioRead(0x303B, 0), 0);
}
