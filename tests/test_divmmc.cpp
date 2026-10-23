#include "test_framework.h"

#include <string>
#include <vector>

#include "core/divmmc.h"
#include "core/machine.h"

using rainbow::DivMmc;
using rainbow::Machine;
using rainbow::MachineType;
using rainbow::u16;
using rainbow::u8;

namespace {

// An 8K image whose bytes identify their own offset, so a read can be checked
// without caring where the block was mapped.
std::vector<u8> makeRom(u8 tag = 0x00) {
  std::vector<u8> rom(DivMmc::RomSize);
  for (size_t i = 0; i < rom.size(); ++i) rom[i] = u8(tag + (i & 0xFF));
  // The signature the Next's boot ROM demands at 0x0004.
  rom[4] = 'D';
  rom[5] = 'V';
  return rom;
}

DivMmc makeLoaded() {
  DivMmc d;
  std::string error;
  CHECK(d.loadRom(makeRom(), error));
  CHECK(error.empty());
  return d;
}

} // namespace

TEST(divmmc_control_latch_is_port_0xe3) {
  DivMmc d = makeLoaded();
  CHECK_EQ(d.control(), 0);
  CHECK(!d.mappedIn());

  u8 value = 0;
  CHECK(d.ioWrite(0xE3, 0x80));
  CHECK(d.ioRead(0xE3, value));
  CHECK_EQ(value, 0x80);
  CHECK(d.mappedIn());

  // The port is decoded on the low byte only, as the Next's is.
  CHECK(d.ioRead(0x12E3, value));
  CHECK(!d.ioRead(0xE4, value));
}

TEST(divmmc_nothing_is_mapped_while_the_latch_is_clear) {
  DivMmc d = makeLoaded();
  u8 value = 0;
  CHECK(!d.read(0x0000, false, 0, value));
  CHECK(!d.read(0x2000, false, 0, value));
  CHECK(!d.write(0x2000, 0x12));
}

TEST(divmmc_maps_rom_and_ram_when_enabled) {
  DivMmc d = makeLoaded();
  d.setControl(DivMmc::Conmem | 5);

  u8 value = 0;
  CHECK(d.read(0x0000, false, 0, value));
  CHECK_EQ(value, 0x00);
  CHECK(d.read(0x0004, false, 0, value));
  CHECK_EQ(value, 'D');
  CHECK(d.read(0x0005, false, 0, value));
  CHECK_EQ(value, 'V');

  // The RAM half answers from the bank the latch names.
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x00);
  CHECK(d.write(0x2000, 0x42));
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x42);

  // A different bank is a different piece of memory.
  d.setControl(DivMmc::Conmem | 6);
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x00);
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x00);

  // Anything above 0x3FFF is not ours.
  CHECK(!d.read(0x4000, false, 0, value));
}

TEST(divmmc_mapram_puts_ram_bank_three_over_the_rom) {
  DivMmc d = makeLoaded();
  d.setControl(DivMmc::Conmem | DivMmc::Mapram | 3);
  u8 value = 0;
  CHECK(d.write(0x0004, 0x99));
  CHECK(d.read(0x0004, false, 0, value));
  CHECK_EQ(value, 0x99);

  d.setControl(DivMmc::Conmem | 3);
  CHECK(d.read(0x0004, false, 0, value));
  CHECK_EQ(value, 'D');
}

TEST(divmmc_rom_half_is_read_only) {
  DivMmc d = makeLoaded();
  d.setControl(DivMmc::Conmem);
  u8 value = 0;
  CHECK(d.write(0x0004, 0x99));
  CHECK(d.read(0x0004, false, 0, value));
  CHECK_EQ(value, 'D');
}

TEST(divmmc_automap_needs_both_the_master_switch_and_a_hook) {
  DivMmc d = makeLoaded();
  u8 value = 0;

  // Automap off: an M1 fetch at a hook address changes nothing. The validity
  // bits are all set so that the ROM3 condition is not what stops it.
  d.setEntryPoints(0xFF, 0xFF, 0x00, 0x00);
  CHECK(!d.read(0x0008, true, 0, value));
  CHECK(!d.held());

  // Automap on but the point not armed.
  d.setAutomap(true);
  d.setEntryPoints(0x00, 0xFF, 0x00, 0x00);
  CHECK(!d.read(0x0008, true, 0, value));
  CHECK(!d.held());

  // Armed and automapping.
  d.setEntryPoints(0x02, 0xFF, 0x00, 0x00);
  d.read(0x0008, true, 0, value);
  d.afterInstruction();
  CHECK(d.held());
}

TEST(divmmc_automap_only_reacts_to_opcode_fetches) {
  DivMmc d = makeLoaded();
  d.setAutomap(true);
  d.setEntryPoints(0x02, 0xFF, 0x00, 0x00);
  u8 value = 0;
  CHECK(!d.read(0x0008, false, 0, value));
  CHECK(!d.held());
}

TEST(divmmc_delayed_hook_maps_in_after_its_own_fetch) {
  DivMmc d = makeLoaded();
  d.setAutomap(true);
  // 0x0000 is armed and valid on any ROM, but 0xBA is clear, so it is delayed:
  // the fetch that trips it still comes from the machine's own memory.
  d.setEntryPoints(0x01, 0x01, 0x00, 0x00);
  u8 value = 0;
  CHECK(!d.read(0x0000, true, 0, value));
  CHECK(!d.shown());
  d.afterInstruction();
  CHECK(d.held());
  // From the next fetch on the block answers, and it stays in after the latch
  // is cleared -- the boot ROM writes 0x00 to port 0xE3 and expects to keep
  // reading its own ROM until it reaches the 0x1FF8 trap.
  CHECK(d.read(0x0001, true, 0, value));
  d.setControl(0);
  CHECK(d.read(0x0001, false, 0, value));
  CHECK(d.shown());
}

TEST(divmmc_instant_hook_answers_its_own_fetch) {
  DivMmc d = makeLoaded();
  d.setAutomap(true);
  d.setEntryPoints(0x01, 0x01, 0x01, 0x00);  // 0xBA bit 0: instant
  u8 value = 0;
  CHECK(d.read(0x0000, true, 0, value));
  CHECK(d.shown());
}

TEST(divmmc_rom3_condition_gates_the_rst_vectors) {
  DivMmc d = makeLoaded();
  d.setAutomap(true);
  // Bit 1 (0x0008) is armed but needs the +3 DOS ROM; bit 7 (0x0038) does not,
  // so only bit 7 is marked valid.
  d.setEntryPoints(0x82, 0x81, 0x00, 0x00);
  u8 value = 0;

  CHECK(!d.read(0x0008, true, 0, value));
  d.afterInstruction();
  CHECK(!d.held());
  d.read(0x0038, true, 0, value);
  d.afterInstruction();
  CHECK(d.held());

  d.setControl(0);
  d.setRom3Present(true);
  d.read(0x0008, true, 0, value);
  d.afterInstruction();
  CHECK(d.held());
}

TEST(divmmc_exit_stub_unmaps_after_the_instruction) {
  DivMmc d = makeLoaded();
  d.setControl(DivMmc::Conmem);
  u8 value = 0;
  // The stub at 0x1FF8 is still served by the divMMC ROM...
  CHECK(d.read(0x1FF8, true, 0x83, value));
  CHECK(d.mappedIn());
  // ...and only lets go once it has run.
  d.afterInstruction();
  CHECK(!d.mappedIn());
  CHECK(!d.shown());

  // The trap is the address, not the accumulator: the boot ROM reaches it with
  // A zeroed and still has to be let go.
  d.setControl(DivMmc::Conmem);
  CHECK(d.read(0x1FF8, true, 0x00, value));
  d.afterInstruction();
  CHECK(!d.mappedIn());

  // Outside 0x1FF8-0x1FFF nothing unmaps.
  d.setControl(DivMmc::Conmem);
  CHECK(d.read(0x1FF7, true, 0x00, value));
  d.afterInstruction();
  CHECK(d.mappedIn());
}

TEST(divmmc_rejects_a_rom_of_the_wrong_size) {
  DivMmc d;
  std::string error;
  CHECK(!d.loadRom(std::vector<u8>(100, 0), error));
  CHECK(!error.empty());
  CHECK(!d.romLoaded());

  error.clear();
  CHECK(d.loadRom(std::vector<u8>(4 * DivMmc::RomSize, 0), error));
  CHECK(d.romLoaded());
  CHECK_EQ(d.romPages(), 4);
}

TEST(divmmc_reset_clears_the_latch_and_only_a_power_cycle_clears_ram) {
  DivMmc d = makeLoaded();
  d.setControl(DivMmc::Conmem | 2);
  d.write(0x2000, 0x77);
  d.softReset();
  CHECK_EQ(d.control(), 0);
  d.setControl(DivMmc::Conmem | 2);
  u8 value = 0;
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x77);

  d.hardReset();
  d.setControl(DivMmc::Conmem | 2);
  CHECK(d.read(0x2000, false, 0, value));
  CHECK_EQ(value, 0x00);
}

TEST(divmmc_the_machine_owns_one_and_the_boot_rom_can_read_it) {
  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.divMmc().loadRom(makeRom(), error));
  CHECK(error.empty());

  // Port 0xE3 maps the block in and the "DV" signature appears at 0x0004,
  // which is exactly the handshake the boot ROM performs at 0x1EDB.
  m.ioWrite(0x00E3, 0x80, 0);
  CHECK_EQ(m.memRead(0x0004, 0), 'D');
  CHECK_EQ(m.memRead(0x0005, 0), 'V');

  // Dropping the latch hands the address back to the machine's own ROM.
  m.ioWrite(0x00E3, 0x00, 0);
  CHECK(m.memRead(0x0004, 0) != 'D' || m.memRead(0x0005, 0) != 'V');
}

TEST(divmmc_nextreg_0x0a_bit_4_is_the_automap_switch) {
  Machine m(MachineType::Next);
  std::string error;
  CHECK(m.divMmc().loadRom(makeRom(), error));
  CHECK(!m.divMmc().automap());

  m.ioWrite(0x243B, 0x0A, 0);
  m.ioWrite(0x253B, 0x10, 0);
  CHECK(m.divMmc().automap());

  m.ioWrite(0x243B, 0x0A, 0);
  m.ioWrite(0x253B, 0x00, 0);
  CHECK(!m.divMmc().automap());
}

TEST(divmmc_nextreg_0xb8_to_0xbb_reach_the_entry_points) {
  Machine m(MachineType::Next);
  m.ioWrite(0x243B, 0xB8, 0);
  m.ioWrite(0x253B, 0x82, 0);
  m.ioWrite(0x243B, 0xB9, 0);
  m.ioWrite(0x253B, 0x01, 0);
  m.ioWrite(0x243B, 0xBB, 0);
  m.ioWrite(0x253B, 0xCD, 0);

  CHECK_EQ(m.divMmc().entryPoints0(), 0x82);
  CHECK_EQ(m.divMmc().entryPointsValid0(), 0x01);
  CHECK_EQ(m.divMmc().entryPoints1(), 0xCD);
}

TEST(divmmc_hook_enable_follows_the_entry_point_registers) {
  DivMmc div;
  div.setEntryPoints(0x82, 0x00, 0x00, 0xCD);

  // 0xB8 asks for 0x0038 and 0x0008 and nothing else, so the reset vector is
  // not an entry point any more. 0xB9 is clear for both, so neither fires
  // unless ROM3 is the one paged in -- that is exactly what keeps the Next
  // boot ROM's own use of RST 8 as a bank switcher working.
  div.setRom3Present(false);
  CHECK(!div.hookEnabled(0x0000));
  CHECK(!div.hookEnabled(0x0038));
  CHECK(!div.hookEnabled(0x0010));
  CHECK(!div.hookEnabled(0x0008));

  div.setRom3Present(true);
  CHECK(div.hookEnabled(0x0038));
  CHECK(div.hookEnabled(0x0008));

  // 0xBB keeps the esxDOS hooks and drops the NMI vectors, which it does by
  // leaving bits 0 and 1 clear.
  div.setEntryPoints(0x00, 0x00, 0x00, 0x04);
  CHECK(!div.hookEnabled(0x0066));
  CHECK(div.hookEnabled(0x04C6));
  CHECK(!div.hookEnabled(0x0562));
}

TEST(next_divmmc_automap_starts_off_and_follows_register_0x0a) {
  Machine machine(MachineType::Next);
  DivMmc* div = &machine.divMmc();

  // A hard reset clears the master switch, so the boot ROM keeps both RST
  // vectors for itself.
  CHECK(!div->automap());

  machine.writeNextReg(0x0A, 0x10);
  CHECK(div->automap());
  machine.writeNextReg(0x0A, 0x00);
  CHECK(!div->automap());
}

TEST(next_divmmc_entry_point_registers_reach_the_module) {
  Machine machine(MachineType::Next);
  DivMmc* div = &machine.divMmc();

  // The soft reset defaults: 0x0000 and 0x0038 everywhere, the rest only with
  // ROM3 paged in.
  CHECK_EQ(div->entryPoints0(), u8(0x83));
  CHECK_EQ(div->entryPointsValid0(), u8(0x01));
  CHECK_EQ(div->entryPointsTiming0(), u8(0x00));
  CHECK_EQ(div->entryPoints1(), u8(0xCD));

  machine.writeNextReg(0xB8, 0x82);
  machine.writeNextReg(0xB9, 0x00);
  machine.writeNextReg(0xBB, 0xF2);
  CHECK_EQ(div->entryPoints0(), u8(0x82));
  CHECK_EQ(div->entryPointsValid0(), u8(0x00));
  CHECK_EQ(div->entryPoints1(), u8(0xF2));
}

TEST(next_divmmc_rst8_only_maps_in_with_rom3_paged) {
  Machine machine(MachineType::Next);
  std::vector<u8> system(0x10000, 0x3F);
  CHECK(machine.loadRomImageData(system.data(), system.size()));
  machine.reset();

  DivMmc* div = &machine.divMmc();
  std::string error;
  CHECK(div->loadRom(std::vector<u8>(DivMmc::RomSize, 0xC9), error));

  // What the Next boot ROM itself programs before it turns the automap on.
  machine.writeNextReg(0x0A, 0x10);
  machine.writeNextReg(0xB8, 0x82);
  machine.writeNextReg(0xB9, 0x00);

  // ROM bank 0 is paged, so the RST 8 vector belongs to the ROM.
  CHECK_EQ(machine.romBank(), u8(0x00));
  CHECK_EQ(machine.memFetch(0x0008, 0), u8(0x3F));
  CHECK(!div->shown());

  // Page the +3 DOS ROM in and the same fetch is a call into esxDOS. ROM 3 is
  // the one that needs both select bits: 0x7FFD bit 4 for the low bit and
  // 0x1FFD bit 2 for the high one.
  machine.ioWrite(0x7FFD, 0x10, 0);
  machine.ioWrite(0x1FFD, 0x04, 0);
  CHECK_EQ(machine.romBank(), u8(0x03));
  // 0xBA bit 0 is clear, so the hook is delayed: the fetch that trips it still
  // answers from the machine's own ROM and the block comes in afterwards.
  CHECK_EQ(machine.memFetch(0x0008, 0), u8(0x3F));
  div->afterInstruction();
  CHECK_EQ(machine.memFetch(0x0008, 0), u8(0xC9));
  // The hook holds the block in through its flip-flop rather than the port
  // 0xE3 latch, which is why the check is on shown() and not mappedIn().
  CHECK(div->shown());
}
