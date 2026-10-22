#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/esxdos.h"
#include "core/z80.h"

using rainbow::Bus;
using rainbow::EsxDos;
using rainbow::u16;
using rainbow::u32;
using rainbow::u8;
using rainbow::Z80;

namespace {

// A flat 64K of RAM. The DOS only needs somewhere to put file names and
// buffers, so a real machine would be more machinery than the test needs.
class FlatBus : public Bus {
public:
  u8 memRead(u16 addr, u32) override { return memory[addr]; }
  void memWrite(u16 addr, u8 value, u32) override { memory[addr] = value; }
  u8 ioRead(u16, u32) override { return 0xFF; }
  void ioWrite(u16, u8, u32) override {}
  u8 contention(u16, u32) override { return 0; }
  void tick(u32) override {}
  u32 now() const override { return 0; }

  void put(u16 address, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) memory[address + i] = u8(text[i]);
    memory[address + text.size()] = 0;
  }

  std::array<u8, 0x10000> memory{};
};

// A directory that cleans itself up, so a failing check cannot leave a mess
// behind in the temporary folder.
class TempDir {
public:
  TempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rainbow-esxdos-" + std::to_string(stamp) + "-" + std::to_string(++counter_));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string str() const { return path_.string(); }

  void file(const std::string& relative, const std::string& contents) const {
    const std::filesystem::path target = path_ / relative;
    std::filesystem::create_directories(target.parent_path());
    std::FILE* f = std::fopen(target.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (f == nullptr) return;
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
  }

private:
  std::filesystem::path path_;
  static int counter_;
};

int TempDir::counter_ = 0;

bool carrySet(const Z80::Registers& regs) { return (regs.f & 0x01) != 0; }

u16 bcOf(const Z80::Registers& regs) { return u16(u16(regs.b) << 8 | regs.c); }

void setBc(Z80::Registers& regs, u16 value) {
  regs.b = u8(value >> 8);
  regs.c = u8(value & 0xFF);
}

// Runs one hook code the way the guest would, returning the carry flag.
bool invoke(EsxDos& dos, Bus& bus, Z80::Registers& regs, u8 code) {
  dos.call(bus, regs, code);
  return carrySet(regs);
}

} // namespace

TEST(EsxDosMountReportsTheRootAndRejectsMissingDirectories) {
  EsxDos dos;
  std::string error;
  CHECK(!dos.mounted());
  CHECK(!dos.mount("/this/directory/does/not/exist", error));
  CHECK(!error.empty());
  CHECK(!dos.mounted());

  TempDir dir;
  CHECK(dos.mount(dir.str(), error));
  CHECK(error.empty());
  CHECK(dos.mounted());
  // The stored root has no trailing separator, so joining is unambiguous.
  CHECK(dos.root() == dir.str());
  CHECK(dos.root().back() != '/');

  dos.unmount();
  CHECK(!dos.mounted());
}

TEST(EsxDosOpensAFileAndReportsItsSize) {
  TempDir dir;
  dir.file("Data/00.bin", std::string("\x01\x02\x03\x04\x05", 5));

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  // The guest writes `./Data/00.bin`, but the host directory is `Data` with a
  // capital D: the lookup has to ignore case for the same image to work on a
  // case-sensitive file system.
  bus.put(0x6000, "./Data/00.bin");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  CHECK_EQ(regs.a, 1); // the console is handle 0, so the first file is 1
  CHECK_EQ(dos.openHandles(), 1);

  // The stat buffer is twelve bytes: drive, device, attributes, time, date and
  // a 32-bit size at offset seven, which is where a loader reads it from.
  regs.a = 1;
  regs.ix = 0x6100;
  CHECK(!invoke(dos, bus, regs, EsxDos::FFstat));
  CHECK_EQ(bus.memory[0x6100], u8(EsxDos::DriveLetter));
  CHECK_EQ(bus.memory[0x6102], 0x20); // archive
  const u32 size = u32(bus.memory[0x6107]) | (u32(bus.memory[0x6108]) << 8) |
                   (u32(bus.memory[0x6109]) << 16) | (u32(bus.memory[0x610A]) << 24);
  CHECK_EQ(size, 5);

  regs.a = 1;
  CHECK(!invoke(dos, bus, regs, EsxDos::FClose));
  CHECK_EQ(dos.openHandles(), 0);
}

TEST(EsxDosReadsIntoIxAndAdvancesThePosition) {
  TempDir dir;
  dir.file("Data/01.bin", "0123456789");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  bus.put(0x6000, "C:/Data/01.bin");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  const u8 handle = regs.a;

  // esxDOS passes the destination in IX, not HL: the loader pushes HL and pops
  // it into IX before every call.
  regs.a = handle;
  regs.ix = 0x8000;
  setBc(regs, 4);
  CHECK(!invoke(dos, bus, regs, EsxDos::FRead));
  CHECK_EQ(bcOf(regs), 4);
  CHECK_EQ(bus.memory[0x8000], u8('0'));
  CHECK_EQ(bus.memory[0x8003], u8('3'));

  regs.a = handle;
  regs.ix = 0x8010;
  setBc(regs, 4);
  CHECK(!invoke(dos, bus, regs, EsxDos::FRead));
  CHECK_EQ(bus.memory[0x8010], u8('4'));

  // Reading past the end returns a short count rather than an error.
  regs.a = handle;
  regs.ix = 0x8020;
  setBc(regs, 16);
  CHECK(!invoke(dos, bus, regs, EsxDos::FRead));
  CHECK_EQ(bcOf(regs), 2);
  CHECK_EQ(bus.memory[0x8020], u8('8'));

  regs.a = handle;
  CHECK(!invoke(dos, bus, regs, EsxDos::FClose));
}

TEST(EsxDosSeeksAndReportsThePosition) {
  TempDir dir;
  dir.file("seek.bin", "abcdefgh");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  bus.put(0x6000, "seek.bin");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  const u8 handle = regs.a;

  // The 32-bit offset goes in BCDE and the origin in L: 0 set, 1 forward,
  // 2 backward.
  regs.a = handle;
  regs.b = 0;
  regs.c = 0;
  regs.d = 0;
  regs.e = 5;
  regs.l = 0;
  CHECK(!invoke(dos, bus, regs, EsxDos::FSeek));

  regs.a = handle;
  regs.ix = 0x8000;
  setBc(regs, 1);
  CHECK(!invoke(dos, bus, regs, EsxDos::FRead));
  CHECK_EQ(bus.memory[0x8000], u8('f'));

  regs.a = handle;
  CHECK(!invoke(dos, bus, regs, EsxDos::FFgetPos));
  const u32 position = (u32(regs.b) << 24) | (u32(regs.c) << 16) |
                       (u32(regs.d) << 8) | u32(regs.e);
  CHECK_EQ(position, 6);

  regs.a = handle;
  CHECK(!invoke(dos, bus, regs, EsxDos::FClose));
}

TEST(EsxDosWritesBackThroughTheCard) {
  TempDir dir;
  dir.file("out.bin", "");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  bus.put(0x6000, "out.bin");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = u8(EsxDos::ModeWrite | EsxDos::ModeTruncate);
  CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  const u8 handle = regs.a;

  bus.put(0x8000, "hello");
  regs.a = handle;
  regs.ix = 0x8000;
  setBc(regs, 5);
  CHECK(!invoke(dos, bus, regs, EsxDos::FWrite));
  CHECK_EQ(bcOf(regs), 5);

  regs.a = handle;
  CHECK(!invoke(dos, bus, regs, EsxDos::FClose));

  // The file has to be there on disk, not just in the handle.
  std::FILE* f = std::fopen((dir.path() / "out.bin").string().c_str(), "rb");
  CHECK(f != nullptr);
  if (f != nullptr) {
    char buffer[8] = {};
    const size_t got = std::fread(buffer, 1, sizeof buffer, f);
    std::fclose(f);
    CHECK_EQ(got, 5);
    CHECK(std::string(buffer, got) == "hello");
  }

  // A handle opened read-only must refuse a write.
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  const u8 readOnly = regs.a;
  regs.ix = 0x8000;
  setBc(regs, 1);
  CHECK(invoke(dos, bus, regs, EsxDos::FWrite));
  CHECK_EQ(regs.a, u8(EsxDos::ErrAcces));
  regs.a = readOnly;
  CHECK(!invoke(dos, bus, regs, EsxDos::FClose));
}

TEST(EsxDosReportsErrorsWithoutCrashing) {
  TempDir dir;
  dir.file("present.bin", "x");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};

  // A missing file.
  bus.put(0x6000, "absent.bin");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(invoke(dos, bus, regs, EsxDos::FOpen));
  CHECK_EQ(regs.a, u8(EsxDos::ErrNoEnt));
  CHECK_EQ(dos.lastError(), u8(EsxDos::ErrNoEnt));

  // A handle that was never opened.
  regs.a = 7;
  CHECK(invoke(dos, bus, regs, EsxDos::FClose));
  CHECK_EQ(regs.a, u8(EsxDos::ErrBadF));

  // A hook code the host does not implement.
  CHECK(invoke(dos, bus, regs, 0xB1));
  CHECK_EQ(regs.a, u8(EsxDos::ErrNoSys));

  // `..` cannot be used to climb out of the mounted directory.
  bus.put(0x6000, "../../etc/passwd");
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;
  CHECK(invoke(dos, bus, regs, EsxDos::FOpen));
  CHECK_EQ(regs.a, u8(EsxDos::ErrNoEnt));
}

TEST(EsxDosAnswersTheVersionAndDriveQueries) {
  TempDir dir;
  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  CHECK(!invoke(dos, bus, regs, EsxDos::MDosVersion));
  CHECK_EQ(regs.a, u8('e')); // "e" is the esxDOS major version letter
  CHECK_EQ(bcOf(regs), 8);

  regs.a = 0;
  CHECK(!invoke(dos, bus, regs, EsxDos::MGetSetDrv));
  CHECK_EQ(regs.a, u8(EsxDos::DriveLetter));

  // The console is always handle 0, which is what a program writes its
  // messages to before it opens anything.
  CHECK(!invoke(dos, bus, regs, EsxDos::MGetHandle));
  CHECK_EQ(regs.a, 0);
}

TEST(EsxDosTrapResumesAfterTheFunctionByte) {
  TempDir dir;
  dir.file("trap.bin", "abc");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));
  dos.setEnabled(true);

  FlatBus bus;
  Z80::Registers regs{};

  // The guest layout: a `RST 8` at 0x9000, its function byte at 0x9001 and the
  // instruction after the call at 0x9002. `RST 8` pushed 0x9000, so the code
  // byte sits at the return address.
  bus.memory[0x9000] = EsxDos::FOpen;
  bus.memory[0x9001] = 0xFF;
  bus.put(0x6000, "trap.bin");
  regs.sp = 0xFF00;
  bus.memory[0xFF00] = 0x00;
  bus.memory[0xFF01] = 0x90;
  regs.pc = 0x0008;
  regs.ix = 0x6000;
  regs.a = u8(EsxDos::DriveLetter);
  regs.b = EsxDos::ModeRead;

  CHECK(dos.trap(bus, regs));
  CHECK_EQ(regs.pc, 0x9001); // past the function byte
  CHECK_EQ(regs.sp, 0xFF02); // the return address has been consumed
  CHECK_EQ(regs.a, 1);
  CHECK(!carrySet(regs));

  // The trap only owns 0x0008.
  regs.pc = 0x0009;
  CHECK(!dos.trap(bus, regs));
  regs.pc = 0x0008;
  dos.setEnabled(false);
  CHECK(!dos.trap(bus, regs));
}

TEST(EsxDosClosesEveryHandleWhenUnmounted) {
  TempDir dir;
  dir.file("a.bin", "a");
  dir.file("b.bin", "b");

  EsxDos dos;
  std::string error;
  CHECK(dos.mount(dir.str(), error));

  FlatBus bus;
  Z80::Registers regs{};
  for (const char* name : {"a.bin", "b.bin"}) {
    bus.put(0x6000, name);
    regs.ix = 0x6000;
    regs.a = u8(EsxDos::DriveLetter);
    regs.b = EsxDos::ModeRead;
    CHECK(!invoke(dos, bus, regs, EsxDos::FOpen));
  }
  CHECK_EQ(dos.openHandles(), 2);
  dos.unmount();
  CHECK_EQ(dos.openHandles(), 0);
}
