#pragma once

#include <array>
#include <cstdio>
#include <string>

#include "core/types.h"
#include "core/z80.h"

namespace rainbow {

class Bus;

// esxDOS, answered by the host instead of by the DivMMC firmware.
//
// Every program reaches the DOS the same way: `RST 8` followed by a one-byte
// function code. On real hardware a DivMMC ROM catches the reset vector, maps
// itself over the bottom 8K and runs a FAT driver out of its own RAM. That
// driver is a separate binary that has to be supplied, booted and kept in sync
// with the card; none of that is needed to answer a game, because the call is
// already fully described by its registers.
//
// So the machine intercepts the fetch at 0x0008 and answers here. One host
// directory stands in for the card, which also makes the whole thing testable
// without a firmware image.
class EsxDos {
public:
  // Hook codes. The names follow esxapi.def.
  static constexpr u8 MDosVersion = 0x88;
  static constexpr u8 MGetSetDrv = 0x89;
  static constexpr u8 MGetHandle = 0x8D;
  static constexpr u8 MGetDate = 0x8E;
  static constexpr u8 FOpen = 0x9A;
  static constexpr u8 FClose = 0x9B;
  static constexpr u8 FSync = 0x9C;
  static constexpr u8 FRead = 0x9D;
  static constexpr u8 FWrite = 0x9E;
  static constexpr u8 FSeek = 0x9F;
  static constexpr u8 FFgetPos = 0xA0;
  static constexpr u8 FFstat = 0xA1;

  // Open modes.
  static constexpr u8 ModeRead = 0x01;
  static constexpr u8 ModeWrite = 0x02;
  static constexpr u8 ModeCreate = 0x08;
  static constexpr u8 ModeTruncate = 0x0C;

  // Error codes, returned in A with carry set.
  static constexpr u8 ErrNoEnt = 5;
  static constexpr u8 ErrIO = 6;
  static constexpr u8 ErrInvArg = 7;
  static constexpr u8 ErrAcces = 8;
  static constexpr u8 ErrBadF = 13;
  static constexpr u8 ErrIsDir = 16;
  static constexpr u8 ErrNameTooLong = 21;
  static constexpr u8 ErrNoSys = 20;

  // The drive letter reported to the guest and the handle of the console
  // stream, which is the one a program gets before it opens anything.
  static constexpr char DriveLetter = 'C';
  static constexpr int MaxHandles = 16;
  static constexpr int MaxPathLength = 256;

  EsxDos();
  ~EsxDos();
  EsxDos(const EsxDos&) = delete;
  EsxDos& operator=(const EsxDos&) = delete;

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }

  // Points the DOS at a host directory. Everything the guest asks for is
  // resolved inside it, so a game cannot reach the rest of the file system.
  bool mount(const std::string& directory, std::string& error);
  void unmount();
  bool mounted() const { return !root_.empty(); }
  const std::string& root() const { return root_; }

  // Answers the call sitting at the top of the stack when the CPU reaches
  // 0x0008, then resumes after the function byte. Returns false when the trap
  // does not apply, so the caller can run the instruction normally.
  bool trap(Bus& bus, Z80::Registers& regs);

  // Runs one hook code with the registers as they are. Exposed so a test can
  // drive the API without a CPU.
  void call(Bus& bus, Z80::Registers& regs, u8 code);

  u8 lastError() const { return lastError_; }
  int openHandles() const;

private:
  struct Entry {
    std::FILE* file = nullptr;
    bool writable = false;
  };

  Entry* handle(u8 number);
  u8 allocate(std::FILE* file, bool writable);
  void closeAll();

  // Guest paths are case-insensitive and accept both separators, so each
  // component is matched against the host directory rather than handed to the
  // host file system as-is.
  std::string resolve(const std::string& guestPath) const;
  std::string readString(Bus& bus, u16 address) const;

  void fail(Z80::Registers& regs, u8 error);
  void succeed(Z80::Registers& regs);

  void dosVersion(Z80::Registers& regs);
  void getSetDrv(Z80::Registers& regs);
  void getHandle(Z80::Registers& regs);
  void getDate(Z80::Registers& regs);
  void open(Bus& bus, Z80::Registers& regs);
  void close(Z80::Registers& regs);
  void read(Bus& bus, Z80::Registers& regs);
  void write(Bus& bus, Z80::Registers& regs);
  void seek(Z80::Registers& regs);
  void getPosition(Z80::Registers& regs);
  void stat(Bus& bus, Z80::Registers& regs);

  std::string root_;
  std::array<Entry, MaxHandles> handles_{};
  u8 lastError_ = 0;
  bool enabled_ = false;
};

} // namespace rainbow
