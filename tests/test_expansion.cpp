#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/expansion.h"
#include "frontend/config.h"
#include "core/machine.h"
#include "peripherals/cartridge.h"
#include "peripherals/kempston.h"
#include "peripherals/kempston_mouse.h"
#include "peripherals/printer.h"
#include "plugin/loader.h"

using mynext::Cartridge;
using mynext::ExpansionBus;
using mynext::HostServices;
using mynext::KempstonJoystick;
using mynext::KempstonMouse;
using mynext::Machine;
using mynext::MachineType;
using mynext::Peripheral;
using mynext::PeripheralInfo;
using mynext::PluginLoader;
using mynext::u16;
using mynext::u32;
using mynext::u8;
using mynext::ZxPrinter;
using mynext::frontend::Config;

namespace {

// A module that claims one port and hands back a fixed byte, so the order the
// bus offers accesses in is visible.
class FakePort final : public Peripheral {
public:
  FakePort(std::string id, u16 port, u8 value) : port_(port), value_(value) {
    info_.id = std::move(id);
    info_.name = info_.id;
  }

  const PeripheralInfo& info() const override { return info_; }
  bool ioRead(u16 port, u32, u8& value) override {
    ++reads;
    if (port != port_) return false;
    value = value_;
    return true;
  }

  int reads = 0;

private:
  PeripheralInfo info_;
  u16 port_;
  u8 value_;
};

HostServices dummyHost() {
  static int token = 0;
  HostServices host;
  host.host = &token;
  return host;
}

std::string tempDir(const char* name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "mynext-tests" / name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

} // namespace

// ---------------------------------------------------------------------------
// The bus itself
// ---------------------------------------------------------------------------

TEST(expansion_first_module_to_claim_a_port_wins) {
  ExpansionBus bus;
  bus.add(std::make_unique<FakePort>("a", 0x1F, 0x11));
  bus.add(std::make_unique<FakePort>("b", 0x1F, 0x22));
  bus.setEnabled("a", true);
  bus.setEnabled("b", true);

  u8 value = 0;
  CHECK(bus.ioRead(0x1F, 0, value));
  CHECK_EQ(value, 0x11);
}

TEST(expansion_passes_unclaimed_ports_down_the_chain) {
  ExpansionBus bus;
  bus.add(std::make_unique<FakePort>("a", 0x1F, 0x11), true);
  bus.add(std::make_unique<FakePort>("b", 0x7F, 0x22), true);

  u8 value = 0;
  CHECK(!bus.ioRead(0x3F, 0, value));
  CHECK(bus.ioRead(0x7F, 0, value));
  CHECK_EQ(value, 0x22);
  // A module sees every access, even the ones it does not claim.
  CHECK_EQ(static_cast<FakePort*>(bus.find("a"))->reads, 2);
}

TEST(expansion_disabled_module_is_skipped) {
  ExpansionBus bus;
  bus.add(std::make_unique<FakePort>("a", 0x1F, 0x11), true);
  bus.add(std::make_unique<FakePort>("b", 0x1F, 0x22));

  u8 value = 0;
  CHECK(bus.ioRead(0x1F, 0, value));
  CHECK_EQ(value, 0x11);
  CHECK_EQ(static_cast<FakePort*>(bus.find("b"))->reads, 0);

  CHECK(bus.setEnabled("a", false));
  CHECK(bus.setEnabled("b", true));
  CHECK(bus.ioRead(0x1F, 0, value));
  CHECK_EQ(value, 0x22);
}

TEST(expansion_enabled_list_round_trips) {
  ExpansionBus bus;
  bus.add(std::make_unique<FakePort>("kempston", 0x1F, 0), true);
  bus.add(std::make_unique<FakePort>("printer", 0xFB, 0), false);
  bus.add(std::make_unique<FakePort>("fuller", 0x7F, 0), true);

  // Order follows the chain, not the order they were switched on.
  CHECK(bus.enabledList() == "kempston,fuller");

  bus.applyEnabledList(" printer ");
  CHECK(!bus.enabled("kempston"));
  CHECK(bus.enabled("printer"));
  CHECK(!bus.enabled("fuller"));
  CHECK(bus.enabledList() == "printer");

  // The list is written back in chain order, whatever order it was given in.
  bus.applyEnabledList("fuller;printer");
  CHECK(bus.enabledList() == "printer,fuller");

  bus.applyEnabledList("");
  CHECK(bus.enabledList().empty());
}

TEST(expansion_observer_fires_when_a_module_changes) {
  ExpansionBus bus;
  int fired = 0;
  bus.setObserver([&fired] { ++fired; });

  bus.add(std::make_unique<FakePort>("a", 0x1F, 0), false);
  CHECK_EQ(fired, 1);
  bus.setEnabled("a", true);
  CHECK_EQ(fired, 2);
  // Setting the same state again is not a change.
  bus.setEnabled("a", true);
  CHECK_EQ(fired, 2);
  bus.remove("a");
  CHECK_EQ(fired, 3);
}

// ---------------------------------------------------------------------------
// Built-in modules, through the machine
// ---------------------------------------------------------------------------

TEST(expansion_kempston_is_plugged_in_by_default) {
  Machine machine(MachineType::Spectrum48);
  CHECK(machine.expansion().enabled("kempston"));

  machine.setKempston(0x0A);
  // Bits 5-7 must stay clear: that is how software detects the interface.
  CHECK_EQ(machine.ioRead(0x1F, 0), 0x0A);
  // A7-A5 are decoded, so every port below 0x20 answers and 0x3F does not.
  CHECK_EQ(machine.ioRead(0x00, 0), 0x0A);
  CHECK_EQ(machine.ioRead(0x3F, 0), 0xFF);

  machine.expansion().setEnabled("kempston", false);
  CHECK_EQ(machine.ioRead(0x1F, 0), 0xFF);
}

TEST(expansion_kempston_mouse_counts_movement) {
  Machine machine(MachineType::Spectrum48);
  CHECK(machine.expansion().setEnabled("kempston-mouse", true));

  auto* mouse = static_cast<KempstonMouse*>(machine.expansion().find("kempston-mouse"));
  CHECK(mouse != nullptr);
  mouse->move(3, 5);
  CHECK_EQ(machine.ioRead(0xFFDF, 0), 3);
  // Y counts up as the pointer goes down, so the register wraps below zero.
  CHECK_EQ(machine.ioRead(0xFDFF, 0), 251);
  // Y is inverted on the wire, and the three buttons read as ones when idle.
  CHECK_EQ(machine.ioRead(0xFBDF, 0), 0xFF);

  mouse->setButtons(0x05);
  CHECK_EQ(machine.ioRead(0xFBDF, 0), 0xFA);

  // The counters wrap rather than saturate.
  mouse->move(-10, 0);
  CHECK_EQ(machine.ioRead(0xFFDF, 0), 249);
}

TEST(expansion_printer_prints_dots_and_exports_pbm) {
  Machine machine(MachineType::Spectrum48);
  CHECK(machine.expansion().setEnabled("printer", true));
  auto* printer = static_cast<ZxPrinter*>(machine.expansion().find("printer"));

  // The printer latches a dot on the falling edge of the strobe, so each dot is
  // one write with the strobe high and one with it low.
  const auto dot = [&machine](bool on) {
    machine.ioWrite(0xFB, u8(on ? 0x86 : 0x06), 0);
    machine.ioWrite(0xFB, u8(on ? 0x84 : 0x04), 0);
  };
  dot(true);
  dot(false);
  dot(true);

  CHECK(printer->used());
  CHECK(printer->motor());
  CHECK_EQ(printer->height(), 1);
  CHECK(printer->pixel(0, 0));
  CHECK(!printer->pixel(1, 0));
  CHECK(printer->pixel(2, 0));

  const std::string pbm = printer->toPbm();
  CHECK(pbm.rfind("P1\n", 0) == 0);
  CHECK(pbm.find("256 1\n") != std::string::npos);
  // First row: "1 0 1" then the rest zeroes.
  CHECK(pbm.find("\n1 0 1 0") != std::string::npos);
}

TEST(expansion_cartridge_takes_over_the_bottom_16k) {
  Machine machine(MachineType::Spectrum48);
  std::vector<u8> rom(0x4000, 0x3F);
  CHECK(machine.loadRomImageData(rom.data(), rom.size()));
  CHECK_EQ(machine.memRead(0x0000, 0), 0x3F);

  auto* cart = static_cast<Cartridge*>(machine.expansion().find("cartridge"));
  CHECK(cart != nullptr);
  std::vector<u8> image(0x4000, 0xAA);
  std::string error;
  CHECK(cart->loadData(image.data(), image.size(), error));
  CHECK(machine.expansion().setEnabled("cartridge", true));

  CHECK_EQ(machine.memRead(0x0000, 0), 0xAA);
  CHECK_EQ(machine.memRead(0x3FFF, 0), 0xAA);
  // The rest of the address space is untouched.
  CHECK_EQ(machine.memRead(0x4000, 0), 0x00);

  CHECK(machine.expansion().setEnabled("cartridge", false));
  CHECK_EQ(machine.memRead(0x0000, 0), 0x3F);
}

TEST(expansion_cartridge_rejects_a_bad_image) {
  Cartridge cart;
  std::string error;
  CHECK(!cart.loadData(nullptr, 0, error));
  CHECK(!error.empty());

  std::vector<u8> huge(Cartridge::PageSize * 5, 0);
  error.clear();
  CHECK(!cart.loadData(huge.data(), huge.size(), error));
  CHECK(!error.empty());
  CHECK(!cart.loaded());
}

// ---------------------------------------------------------------------------
// Plugin loader
// ---------------------------------------------------------------------------

TEST(expansion_plugin_loader_reports_a_missing_folder) {
  const auto result = PluginLoader::scan(tempDir("empty") + "/nope", dummyHost());
  CHECK(result.modules.empty());
  CHECK_EQ(result.errors.size(), size_t(1));
}

TEST(expansion_plugin_loader_survives_a_broken_library) {
  const std::string dir = tempDir("broken");
  const std::vector<std::string> extensions = PluginLoader::extensions();
  for (const std::string& extension : extensions) {
    std::ofstream out(dir + "/broken." + extension, std::ios::binary);
    out << "this is not a shared library";
  }
  // A file that is not a plugin at all is ignored, not reported.
  {
    std::ofstream out(dir + "/notes.txt");
    out << "hello";
  }

  const auto result = PluginLoader::scan(dir, dummyHost());
  CHECK(result.modules.empty());
  CHECK_EQ(result.errors.size(), extensions.size());
}

#ifdef MYNEXT_PLUGIN_DIR
TEST(expansion_plugin_loader_finds_the_example_plugin) {
  static int token = 0;
  HostServices host;
  host.host = &token;
  host.joystick = [](void*) -> u8 { return 0x05; };  // right + down

  const auto result = PluginLoader::scan(MYNEXT_PLUGIN_DIR, host);
  CHECK_EQ(result.errors.size(), size_t(0));

  mynext::Peripheral* fuller = nullptr;
  for (const auto& module : result.modules) {
    if (module->info().id == "fuller") fuller = module.get();
  }
  CHECK(fuller != nullptr);
  if (fuller == nullptr) return;

  CHECK(fuller->info().name == "Fuller Box Joystick");
  u8 value = 0;
  CHECK(fuller->ioRead(0x7F, 0, value));
  CHECK_EQ(value, 0xE5);
  // Mirrors of the port answer too, anything else is passed on.
  CHECK(fuller->ioRead(0xFF7F, 0, value));
  CHECK(!fuller->ioRead(0x1F, 0, value));
}
#endif

// ---------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------

TEST(expansion_config_round_trips) {
  const std::string path = tempDir("config") + "/mynext.cfg";
  Config saved;
  saved.pluginDir = "/tmp/peripherals";
  saved.enabledPeripherals = {"kempston", "printer"};

  CHECK(saved.save(path));
  Config loaded;
  CHECK(loaded.load(path));
  CHECK(loaded.pluginDir == saved.pluginDir);
  CHECK_EQ(loaded.enabledPeripherals.size(), size_t(2));
  CHECK(loaded.enabledPeripherals[0] == "kempston");
  CHECK(loaded.enabledPeripherals[1] == "printer");
}

TEST(expansion_config_ignores_junk_and_missing_files) {
  const std::string dir = tempDir("config-junk");
  {
    std::ofstream out(dir + "/mynext.cfg");
    out << "# a comment\n";
    out << "\n";
    out << "not a setting\n";
    out << "unknown = 1\n";
    out << "peripherals = kempston , fuller\n";
  }

  Config config;
  CHECK(config.load(dir + "/mynext.cfg"));
  CHECK_EQ(config.enabledPeripherals.size(), size_t(2));
  CHECK(config.enabledPeripherals[1] == "fuller");

  Config missing;
  CHECK(!missing.load(dir + "/nope.cfg"));
  CHECK(missing.enabledPeripherals.empty());
}
