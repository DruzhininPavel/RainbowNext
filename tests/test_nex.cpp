#include "test_framework.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/machine.h"
#include "core/palette.h"
#include "formats/nex.h"

using rainbow::Machine;
using rainbow::MachineType;
using rainbow::NextPalette;
using rainbow::u16;
using rainbow::u8;
using rainbow::formats::loadNex;
using rainbow::formats::NexFile;

namespace {

constexpr size_t HeaderSize = 512;
constexpr size_t BankSize = 16384;

void putWord(std::vector<u8>& v, size_t offset, u16 value) {
  v[offset] = u8(value & 0xFF);
  v[offset + 1] = u8(value >> 8);
}

// Builds a header plus whatever payload the caller appends. Only the fields the
// tests care about are set; the rest stay zero, which is what a minimal .nex
// looks like.
std::vector<u8> makeNex(const std::vector<int>& banks, u16 pc, u16 sp, u8 entryBank) {
  std::vector<u8> file(HeaderSize, 0);
  file[0] = 'N';
  file[1] = 'e';
  file[2] = 'x';
  file[3] = 't';
  file[4] = 'V';
  file[5] = '1';
  file[6] = '.';
  file[7] = '2';
  file[8] = 1; // 1792K
  file[9] = u8(banks.size());
  putWord(file, 12, sp);
  putWord(file, 14, pc);
  for (const int bank : banks) file[size_t(18 + bank)] = 1;
  file[139] = entryBank;
  return file;
}

// Each bank is filled with a byte that identifies it, so a mis-ordered or
// misplaced bank is visible in the loaded memory.
void appendBank(std::vector<u8>& file, int bank) {
  for (size_t i = 0; i < BankSize; ++i) file.push_back(u8(bank * 3 + 1));
}

std::string tempDir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "rainbow-tests" / "nex";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

std::string writeTemp(const std::string& name, const std::vector<u8>& bytes) {
  const std::string path = tempDir() + "/" + name;
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file != nullptr) {
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
  }
  return path;
}

} // namespace

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

TEST(nex_parses_a_minimal_file) {
  std::vector<u8> file = makeNex({5}, 0x8000, 0x5B00, 5);
  appendBank(file, 5);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK(error.empty());
  CHECK_EQ(nex.major, 1);
  CHECK_EQ(nex.minor, 2);
  CHECK_EQ(nex.ramRequired, 1);
  CHECK_EQ(nex.bankCount, 1);
  CHECK_EQ(nex.pc, 0x8000);
  CHECK_EQ(nex.sp, 0x5B00);
  CHECK_EQ(nex.entryBank, 5);
  CHECK(nex.bankPresent[5]);
  CHECK(!nex.bankPresent[0]);
  CHECK_EQ(nex.banks.size(), 1);
  CHECK_EQ(nex.banks[0].number, 5);
  CHECK_EQ(nex.banks[0].data.size(), BankSize);
  CHECK_EQ(nex.banks[0].data[0], 16);
  CHECK_EQ(nex.banks[0].data[BankSize - 1], 16);
}

TEST(nex_parses_the_remaining_header_fields) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x02; // ULA screen, which needs no palette block
  file[11] = 0x05;
  file[130] = 1;    // loading bar
  file[131] = 0x03; // its colour
  file[132] = 4;    // per-bank delay
  file[133] = 200;  // start delay
  file[134] = 1;    // preserve NextRegs
  file[135] = 3;
  file[136] = 1;
  file[137] = 5;
  file[138] = 0x28; // Timex colour 5 in bits 5:3
  putWord(file, 140, 0x5B00);
  file[142] = 0;
  for (size_t i = 0; i < 6912; ++i) file.push_back(0);
  appendBank(file, 0);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK_EQ(nex.screenFlags, 0x02);
  CHECK_EQ(nex.border, 5);
  CHECK_EQ(nex.loadingBar, 1);
  CHECK_EQ(nex.loadingBarColour, 3);
  CHECK_EQ(nex.loadingDelay, 4);
  CHECK_EQ(nex.startDelay, 200);
  CHECK(nex.preserveNextRegs);
  CHECK_EQ(nex.coreMajor, 3);
  CHECK_EQ(nex.coreMinor, 1);
  CHECK_EQ(nex.coreSubminor, 5);
  CHECK_EQ(nex.timexColour, 0x28);
  CHECK_EQ(nex.fileHandleAddress, 0x5B00);
  CHECK_EQ(nex.expBusDisable, 0);
}

TEST(nex_masks_the_border_to_three_bits) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[11] = 0xFF;
  appendBank(file, 0);
  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK_EQ(nex.border, 7);
}

// ---------------------------------------------------------------------------
// Bank order
// ---------------------------------------------------------------------------

TEST(nex_reads_banks_in_loader_order) {
  // 5, 2 and 0 first because the loader can show a screen straight from them,
  // then everything else in ascending order.
  std::vector<u8> file = makeNex({0, 1, 2, 3, 4, 5, 7}, 0, 0, 0);
  for (const int bank : {5, 2, 0, 1, 3, 4, 7}) appendBank(file, bank);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK_EQ(nex.banks.size(), 7);
  const int expected[] = {5, 2, 0, 1, 3, 4, 7};
  for (size_t i = 0; i < nex.banks.size(); ++i) {
    CHECK_EQ(nex.banks[i].number, expected[i]);
    CHECK_EQ(nex.banks[i].data[0], u8(expected[i] * 3 + 1));
  }
}

TEST(nex_skips_banks_that_are_not_flagged) {
  std::vector<u8> file = makeNex({6, 9}, 0, 0, 0);
  appendBank(file, 6);
  appendBank(file, 9);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK_EQ(nex.banks.size(), 2);
  CHECK_EQ(nex.banks[0].number, 6);
  CHECK_EQ(nex.banks[1].number, 9);
}

TEST(nex_ignores_trailing_bytes) {
  // Real files carry a raw program past the last bank; the loader must not care.
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  appendBank(file, 0);
  file.push_back(0xAA);
  file.push_back(0xBB);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK_EQ(nex.banks.size(), 1);
}

// ---------------------------------------------------------------------------
// Loading screen and palette
// ---------------------------------------------------------------------------

TEST(nex_reads_the_palette_before_the_layer2_screen) {
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  file[10] = 0x01; // Layer 2 loading screen, palette present
  for (int entry = 0; entry < 256; ++entry) {
    file.push_back(u8(entry));      // RRRGGGBB
    file.push_back(u8(entry & 1));  // the ninth bit
  }
  for (size_t i = 0; i < 49152; ++i) file.push_back(u8(i & 0xFF));
  appendBank(file, 0);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK(nex.hasPalette);
  // The ninth bit is the low bit of the three-bit blue channel, so index 1 is
  // RRRGGGBB=00000001 plus that bit: blue becomes 11b.
  CHECK_EQ(nex.palette[0x00], 0x000);
  CHECK_EQ(nex.palette[0x01], 0x003);
  CHECK_EQ(nex.palette[0x55], 0x0AB);
  CHECK_EQ(nex.palette[0xFF], 0x1FF);
  CHECK_EQ(nex.layer2Screen.size(), 49152);
  CHECK_EQ(nex.layer2Screen[0x1234], 0x34);
  CHECK_EQ(nex.banks.size(), 1);
}

TEST(nex_skips_the_palette_when_bit_seven_says_so) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x81; // Layer 2 screen but no palette block
  for (size_t i = 0; i < 49152; ++i) file.push_back(u8(i & 0xFF));
  appendBank(file, 0);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK(!nex.hasPalette);
  CHECK_EQ(nex.layer2Screen.size(), 49152);
  CHECK_EQ(nex.banks.size(), 1);
}

TEST(nex_ula_screen_has_no_palette_block) {
  // The ULA palette is fixed, so the file has no palette even though bit 7 is
  // clear.
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x02;
  for (size_t i = 0; i < 6912; ++i) file.push_back(u8(i & 0xFF));
  appendBank(file, 0);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK(!nex.hasPalette);
  CHECK_EQ(nex.ulaScreen.size(), 6912);
  CHECK_EQ(nex.ulaScreen[0x1AFF], 0xFF);
  CHECK_EQ(nex.banks.size(), 1);
}

TEST(nex_reads_every_screen_block_in_order) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x1F; // Layer 2, ULA, LoRes, HiRes and HiCol
  for (size_t i = 0; i < 49152; ++i) file.push_back(0x11);
  for (size_t i = 0; i < 6912; ++i) file.push_back(0x22);
  for (size_t i = 0; i < 12288; ++i) file.push_back(0x33);
  for (size_t i = 0; i < 12288; ++i) file.push_back(0x44);
  for (size_t i = 0; i < 12288; ++i) file.push_back(0x55);
  appendBank(file, 0);

  NexFile nex;
  std::string error;
  CHECK(loadNex(file, nex, error));
  CHECK(!nex.hasPalette);
  CHECK_EQ(nex.layer2Screen[0], 0x11);
  CHECK_EQ(nex.ulaScreen[0], 0x22);
  CHECK_EQ(nex.loresScreen[0], 0x33);
  CHECK_EQ(nex.hiresScreen[0], 0x44);
  CHECK_EQ(nex.hicolScreen[0], 0x55);
  CHECK_EQ(nex.banks.size(), 1);
}

// ---------------------------------------------------------------------------
// Rejected files
// ---------------------------------------------------------------------------

TEST(nex_rejects_a_missing_signature) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[0] = 'X';
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("signature") != std::string::npos);
}

TEST(nex_rejects_a_short_header) {
  std::vector<u8> file(64, 0);
  std::memcpy(file.data(), "NextV1.2", 8);
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("header") != std::string::npos);
}

TEST(nex_rejects_a_future_version) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[5] = '2';
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("version") != std::string::npos);
}

TEST(nex_rejects_an_out_of_range_entry_bank) {
  std::vector<u8> file = makeNex({0}, 0, 0, 200);
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("entry bank") != std::string::npos);
}

TEST(nex_rejects_a_truncated_bank) {
  std::vector<u8> file = makeNex({0, 5}, 0, 0, 0);
  appendBank(file, 5);
  file.resize(file.size() + 1000); // the second bank stops short
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("truncated") != std::string::npos);
}

TEST(nex_rejects_a_truncated_loading_screen) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x02;
  file.resize(file.size() + 100);
  NexFile nex;
  std::string error;
  CHECK(!loadNex(file, nex, error));
  CHECK(error.find("ULA loading screen") != std::string::npos);
}

TEST(nex_reports_a_missing_file) {
  Machine m(MachineType::Next);
  std::string error;
  CHECK(!m.loadNex(tempDir() + "/does-not-exist.nex", error));
  CHECK(error.find("cannot open") != std::string::npos);
}

TEST(nex_needs_a_next) {
  Machine m(MachineType::Spectrum48);
  std::string error;
  CHECK(!m.loadNex(tempDir() + "/does-not-exist.nex", error));
  CHECK(error.find("Next") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Applying a file to the machine
// ---------------------------------------------------------------------------

TEST(nex_loads_memory_and_starts_at_pc) {
  std::vector<u8> file = makeNex({5, 2, 0, 9}, 0x8000, 0x5B00, 9);
  appendBank(file, 5);
  appendBank(file, 2);
  appendBank(file, 0);
  appendBank(file, 9);
  const std::string path = writeTemp("start.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK(error.empty());

  CHECK_EQ(m.cpu().regs().pc, 0x8000);
  CHECK_EQ(m.cpu().regs().sp, 0x5B00);
  CHECK(!m.cpu().regs().iff1);
  CHECK(!m.cpu().regs().iff2);

  // Bank 5 is the default mapping at 0x4000 and bank 2 at 0x8000.
  CHECK_EQ(m.memRead(0x4000, 0), 16);  // bank 5
  CHECK_EQ(m.memRead(0x8000, 0), 7);   // bank 2
  // The entry bank lands at 0xC000 through MMU slots 6 and 7.
  CHECK_EQ(m.nextRegs().mmuSlot(6), 18);
  CHECK_EQ(m.nextRegs().mmuSlot(7), 19);
  CHECK_EQ(m.memRead(0xC000, 0), 28);  // bank 9
  CHECK_EQ(m.memRead(0xFFFF, 0), 28);
}

TEST(nex_places_the_layer2_screen_in_banks_nine_to_eleven) {
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  file[10] = 0x81; // Layer 2 screen, no palette
  for (size_t i = 0; i < 49152; ++i) file.push_back(u8(i / 16384 + 1));
  appendBank(file, 0);
  const std::string path = writeTemp("layer2.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  // Walk banks 9, 10 and 11 by mapping each into slot 6.
  for (int bank = 9; bank <= 11; ++bank) {
    m.writeNextReg(0x56, u8(bank * 2));
    m.writeNextReg(0x57, u8(bank * 2 + 1));
    CHECK_EQ(m.memRead(0xC000, 0), bank - 8);
    CHECK_EQ(m.memRead(0xFFFF, 0), bank - 8);
  }
}

TEST(nex_places_the_ula_screen_in_bank_five) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x02;
  for (size_t i = 0; i < 6912; ++i) file.push_back(0xC0);
  appendBank(file, 0);
  const std::string path = writeTemp("ula.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.memRead(0x4000, 0), 0xC0);
  CHECK_EQ(m.memRead(0x4000 + 6911, 0), 0xC0);
  // The rest of bank 5 is untouched by the screen block.
  CHECK_EQ(m.memRead(0x4000 + 6912, 0), 0);
}

TEST(nex_a_bank_overwrites_the_loading_screen_it_shares) {
  // Bank 5 carries both the ULA screen and (usually) the program's own data;
  // the loader writes the banks last, so the bank wins.
  std::vector<u8> file = makeNex({5}, 0x8000, 0, 0);
  file[10] = 0x02;
  for (size_t i = 0; i < 6912; ++i) file.push_back(0xC0);
  appendBank(file, 5);
  const std::string path = writeTemp("overlap.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.memRead(0x4000, 0), 16);
}

TEST(nex_applies_the_palette_to_the_layer2_first_palette) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[10] = 0x01;
  for (int entry = 0; entry < 256; ++entry) {
    file.push_back(u8(entry));
    file.push_back(u8(entry & 1));
  }
  for (size_t i = 0; i < 49152; ++i) file.push_back(0);
  appendBank(file, 0);
  const std::string path = writeTemp("palette.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  const NextPalette& palette = m.palette();
  CHECK_EQ(palette.colour9(NextPalette::Layer2First, 0x00), 0x000);
  CHECK_EQ(palette.colour9(NextPalette::Layer2First, 0x01), 0x003);
  CHECK_EQ(palette.colour9(NextPalette::Layer2First, 0x55), 0x0AB);
  CHECK_EQ(palette.colour9(NextPalette::Layer2First, 0xFF), 0x1FF);
}

TEST(nex_sets_the_border) {
  std::vector<u8> file = makeNex({0}, 0, 0, 0);
  file[11] = 4;
  appendBank(file, 0);
  const std::string path = writeTemp("border.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.border(), 4);
}

TEST(nex_leaves_the_loader_register_state_behind) {
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  appendBank(file, 0);
  const std::string path = writeTemp("regs.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.readNextReg(0x07), 0x03); // 28 MHz
  CHECK_EQ(m.readNextReg(0x12), 0x09); // Layer 2 bank
  CHECK_EQ(m.readNextReg(0x14), 0xE3); // transparency index
  CHECK_EQ(m.nextRegs().mmuSlot(0), 0xFF);
  CHECK_EQ(m.nextRegs().mmuSlot(1), 0xFF);
}

TEST(nex_resets_a_machine_that_was_already_running) {
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  appendBank(file, 0);
  const std::string path = writeTemp("reset.nex", file);

  Machine m(MachineType::Next);
  m.runFrame();
  CHECK(m.frames() > 0);
  m.writeNextReg(0x12, 0x2A);

  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.frames(), 0);
  CHECK_EQ(m.readNextReg(0x12), 0x09); // the loader's value, not ours
}

TEST(nex_preserve_next_registers_keeps_them) {
  std::vector<u8> file = makeNex({0}, 0x8000, 0, 0);
  file[134] = 1; // preserve
  appendBank(file, 0);
  const std::string path = writeTemp("preserve.nex", file);

  Machine m(MachineType::Next);
  m.writeNextReg(0x12, 0x2A);
  std::string error;
  CHECK(m.loadNex(path, error));
  // reset() still runs, so the register file goes back to its reset state rather
  // than the loader's; what matters is that 0x12 is not forced to 9.
  CHECK_EQ(m.readNextReg(0x12), 0x08);
}

TEST(nex_zero_pc_means_load_without_running) {
  std::vector<u8> file = makeNex({0}, 0, 0x5B00, 0);
  appendBank(file, 0);
  const std::string path = writeTemp("nopc.nex", file);

  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.loadNex(path, error));
  CHECK_EQ(m.cpu().regs().pc, 0);
  CHECK_EQ(m.cpu().regs().sp, 0x5B00);
}
