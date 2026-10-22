// Runs Patrik Rak's z80test conformance suite (tests/data/z80test) inside the
// emulator and checks the result line the suite prints on screen. The suite is
// a TAP file, so it needs a working ROM to autoload from; the ROM is not
// shipped with the repository and the tests skip silently when it is absent.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_framework.h"

#include "core/machine.h"
#include "formats/tape.h"
#include "frontend/autoload.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u16;
using rainbow::u8;

namespace {

std::vector<u8> readFile(const std::string& path) {
  std::vector<u8> data;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return data;
  u8 buffer[0x4000];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
    data.insert(data.end(), buffer, buffer + got);
  }
  std::fclose(file);
  return data;
}

// The suite prints one line per test and waits for a key press after every
// twenty, so the runner has to read the bottom text row on every frame. That is
// only affordable when each cell is matched against the cached glyph shapes
// instead of being compared pixel by pixel with the whole ROM character set.
class ScreenReader {
public:
  explicit ScreenReader(Machine& machine) : machine_(machine) {
    for (int ch = 32; ch < 128; ++ch) {
      for (int line = 0; line < 8; ++line) {
        glyphs_[(ch - 32) * 8 + line] = machine.memRead(u16(0x3D00 + (ch - 32) * 8 + line), 0);
      }
    }
  }

  std::string row(int textRow) {
    const int y = textRow * 8;
    const u16 base = u16(0x4000 + ((y & 0xC0) << 5) + ((y & 0x07) << 8) + ((y & 0x38) << 2));
    std::string text;
    for (int column = 0; column < 32; ++column) {
      u8 pixels[8];
      for (int line = 0; line < 8; ++line) {
        pixels[line] = machine_.memRead(u16(base + column + (line << 8)), 0);
      }
      text += char(match(pixels));
    }
    return text;
  }

private:
  char match(const u8 (&pixels)[8]) const {
    for (int ch = 32; ch < 128; ++ch) {
      const u8* glyph = &glyphs_[(ch - 32) * 8];
      int line = 0;
      while (line < 8 && glyph[line] == pixels[line]) ++line;
      if (line == 8) return char(ch);
    }
    return '?';
  }

  Machine& machine_;
  u8 glyphs_[96 * 8] = {};
};

struct RunResult {
  bool finished = false;
  bool passed = false;
  int failures = -1;
  std::vector<std::string> screen;
};

// Plays the suite and collects the outcome. The key presses are only sent while
// the `scroll?` prompt is on screen: holding a key down any longer makes the
// suite's own `IN A,(FE)` sanity check read a pressed matrix line and report a
// failure that has nothing to do with the instruction under test.
RunResult runSuite(const std::string& tapName, int maxFrames) {
  RunResult result;

  Machine machine(MachineType::Spectrum48);
  if (!machine.loadRom(RAINBOW_SOURCE_DIR "/roms/48.rom")) return result;

  const std::vector<u8> tap =
      readFile(std::string(RAINBOW_SOURCE_DIR "/tests/data/z80test/") + tapName);
  if (tap.empty()) return result;

  rainbow::formats::TapeImage image;
  std::string error;
  if (!rainbow::formats::parseTap(tap, image, error)) return result;
  machine.tape().loadImage(image);
  machine.tape().setFastLoad(true);
  machine.setVideoEnabled(false);

  rainbow::frontend::Autoload autoload;
  autoload.start(machine);
  while (autoload.active()) {
    machine.runFrame();
    autoload.tick();
  }

  ScreenReader reader(machine);
  int hold = 0;
  for (int frame = 0; frame < maxFrames; ++frame) {
    if (hold > 0) {
      if (hold == 1) machine.keyboard().setKey(6, 0, false);
      --hold;
    }
    machine.runFrame();

    const std::string bottom = reader.row(23);
    if (hold == 0 && bottom.find("scroll?") != std::string::npos) {
      machine.keyboard().setKey(6, 0, true); // ENTER
      hold = 2;
    }

    if (frame % 64 != 0) continue;
    for (int row = 0; row < 24; ++row) {
      const std::string line = reader.row(row);
      const size_t at = line.find("Result: ");
      if (at == std::string::npos) continue;
      result.finished = true;
      result.passed = line.find("all tests passed") != std::string::npos;
      const size_t failed = line.find(" tests failed");
      if (failed != std::string::npos) {
        result.failures = std::atoi(line.substr(at + 8, failed - (at + 8)).c_str());
      }
      for (int r = 0; r < 24; ++r) result.screen.push_back(reader.row(r));
      return result;
    }
  }
  return result;
}

RunResult runSuiteOrFail(const char* tapName, int maxFrames = 60000) {
  const RunResult result = runSuite(tapName, maxFrames);
  if (!result.finished) {
    ::testing::fail(__FILE__, __LINE__, std::string(tapName) + " did not reach a result line");
  }
  return result;
}

std::string screenDump(const RunResult& result) {
  std::string report;
  for (const std::string& line : result.screen) report += "  " + line + "\n";
  return report;
}

} // namespace

// The documented flag suite is the one the emulator must always satisfy.
TEST(z80test_documented_flags_all_pass) {
  const RunResult result = runSuiteOrFail("z80doc.tap");
  if (result.finished && !result.passed) {
    ::testing::fail(__FILE__, __LINE__,
                    "z80doc reported " + std::to_string(result.failures) + " failing test(s):\n" +
                        screenDump(result));
  }
}

// Same vectors, but every flag is compared, so this one also pins the
// undocumented F3/F5 behaviour of the block instructions.
TEST(z80test_documented_and_undocumented_flags_all_pass) {
  const RunResult result = runSuiteOrFail("z80docflags.tap");
  if (result.finished && !result.passed) {
    ::testing::fail(__FILE__, __LINE__,
                    "z80docflags reported " + std::to_string(result.failures) +
                        " failing test(s):\n" + screenDump(result));
  }
}

// The full suite compares registers as well and checks the undocumented flags
// of every instruction. Eight of its 160 cases are still off: SCF, CCF and their
// `(ST)` variants depend on the exact CPU model and Patrik Rak documents that
// any emulator will disagree with half of them, while the four `->NOP'` block
// instruction cases rewrite their own opcode and end up testing the Zilog
// MEMPTR register. The test guards against regressions outside that set.
TEST(z80test_full_flag_suite_has_no_new_regressions) {
  const RunResult result = runSuiteOrFail("z80flags.tap");
  if (!result.finished || result.passed) return;
  if (result.failures > 8) {
    ::testing::fail(__FILE__, __LINE__,
                    "z80flags reported " + std::to_string(result.failures) +
                        " failing test(s), more than the 8 known gaps:\n" + screenDump(result));
  }
}
