#include "core/esxdos.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "core/bus.h"

namespace rainbow {
namespace {

constexpr u8 FlagCarry = 0x01;

// esxDOS keeps the drive letter in A and the attributes in the stat buffer, so
// a few FAT conventions leak into the answers.
constexpr u8 AttrArchive = 0x20;

bool sameName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// The guest writes a FAT path, which is case-insensitive; macOS is too, but
// Linux and the Steam Deck are not, so the match has to be made here.
std::string matchInDirectory(const std::string& directory, const std::string& name) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) break;
    const std::string candidate = entry.path().filename().string();
    if (sameName(candidate, name)) return candidate;
  }
  return {};
}

void writeWord(Bus& bus, u16 address, u16 value) {
  bus.memWrite(address, u8(value & 0xFF), 0);
  bus.memWrite(u16(address + 1), u8(value >> 8), 0);
}

u16 deOf(const Z80::Registers& regs) { return u16(u16(regs.d) << 8 | regs.e); }

void setBc(Z80::Registers& regs, u16 value) {
  regs.b = u8(value >> 8);
  regs.c = u8(value & 0xFF);
}

} // namespace

EsxDos::EsxDos() = default;

EsxDos::~EsxDos() { closeAll(); }

bool EsxDos::mount(const std::string& directory, std::string& error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    error = "not a directory: " + directory;
    return false;
  }
  unmount();
  // The trailing separator is dropped so that resolve() can build paths by
  // appending "/name" without doubling it.
  std::string root = std::filesystem::absolute(directory, ec).lexically_normal().string();
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  root_ = root;
  return true;
}

void EsxDos::unmount() {
  closeAll();
  root_.clear();
}

int EsxDos::openHandles() const {
  int count = 0;
  for (int i = 1; i < MaxHandles; ++i)
    if (handles_[size_t(i)].file != nullptr) ++count;
  return count;
}

void EsxDos::closeAll() {
  for (auto& entry : handles_) {
    if (entry.file != nullptr) std::fclose(entry.file);
    entry = Entry{};
  }
}

EsxDos::Entry* EsxDos::handle(u8 number) {
  if (number == 0 || number >= MaxHandles) return nullptr;
  Entry& entry = handles_[number];
  return entry.file != nullptr ? &entry : nullptr;
}

u8 EsxDos::allocate(std::FILE* file, bool writable) {
  for (int i = 1; i < MaxHandles; ++i) {
    if (handles_[size_t(i)].file == nullptr) {
      handles_[size_t(i)] = Entry{file, writable};
      return u8(i);
    }
  }
  return 0xFF;
}

std::string EsxDos::readString(Bus& bus, u16 address) const {
  std::string text;
  for (int i = 0; i < MaxPathLength; ++i) {
    const u8 c = bus.memRead(u16(address + u16(i)), 0);
    if (c == 0) break;
    text.push_back(char(c));
  }
  return text;
}

std::string EsxDos::resolve(const std::string& guestPath) const {
  if (root_.empty()) return {};
  size_t i = 0;
  // `C:` and `C` both name the one drive there is.
  if (guestPath.size() >= 2 && guestPath[1] == ':') i = 2;
  else if (!guestPath.empty() && guestPath[0] == DriveLetter && guestPath.size() > 1 &&
           (guestPath[1] == '/' || guestPath[1] == '\\'))
    i = 1;

  std::string path = root_;
  bool atRoot = true;
  while (i < guestPath.size()) {
    while (i < guestPath.size() && (guestPath[i] == '/' || guestPath[i] == '\\')) ++i;
    const size_t start = i;
    while (i < guestPath.size() && guestPath[i] != '/' && guestPath[i] != '\\') ++i;
    if (i == start) break;
    const std::string part = guestPath.substr(start, i - start);
    if (part == ".") continue;
    if (part == "..") {
      if (atRoot) return {}; // never climb out of the mounted folder
      const size_t slash = path.find_last_of('/');
      path.erase(slash == std::string::npos ? 0 : slash);
      atRoot = path.size() <= root_.size();
      continue;
    }
    std::string next = path + "/" + part;
    std::error_code ec;
    if (!std::filesystem::exists(next, ec)) {
      const std::string match = matchInDirectory(path, part);
      if (match.empty()) return {};
      next = path + "/" + match;
    }
    path = next;
    atRoot = false;
  }
  return path;
}

void EsxDos::fail(Z80::Registers& regs, u8 error) {
  lastError_ = error;
  regs.a = error;
  regs.f |= FlagCarry;
}

void EsxDos::succeed(Z80::Registers& regs) {
  lastError_ = 0;
  regs.f &= u8(~FlagCarry);
}

bool EsxDos::trap(Bus& bus, Z80::Registers& regs) {
  if (!enabled_ || regs.pc != 0x0008) return false;
  const u16 sp = regs.sp;
  const u16 callAddress = u16(bus.memRead(sp, 0) | (bus.memRead(u16(sp + 1), 0) << 8));
  const u8 code = bus.memRead(callAddress, 0);
  // The call consumed its own byte, so the program resumes one past it.
  regs.sp = u16(sp + 2);
  regs.pc = u16(callAddress + 1);
  call(bus, regs, code);
  return true;
}

void EsxDos::call(Bus& bus, Z80::Registers& regs, u8 code) {
  ++callCount_;
  lastCall_ = code;
  switch (code) {
    case MDosVersion: dosVersion(regs); break;
    case MGetSetDrv: getSetDrv(regs); break;
    case MGetHandle: getHandle(regs); break;
    case MGetDate: getDate(regs); break;
    case FOpen: open(bus, regs); break;
    case FClose: close(regs); break;
    case FSync: {
      // Writes go straight to the host file, so there is never anything left to
      // flush; a program that asks anyway gets the handle back untouched.
      if (handle(regs.a) == nullptr) fail(regs, ErrBadF);
      else succeed(regs);
      break;
    }
    case FRead: read(bus, regs); break;
    case FWrite: write(bus, regs); break;
    case FSeek: seek(regs); break;
    case FFgetPos: getPosition(regs); break;
    case FFstat: stat(bus, regs); break;
    default: fail(regs, ErrNoSys); break;
  }
}

void EsxDos::dosVersion(Z80::Registers& regs) {
  // esxDOS reports itself with 'e' in A, a BCD version in BC and the address of
  // the version string in HL. Programs mostly test A.
  regs.a = 'e';
  setBc(regs, 0x0008);
  regs.h = 0;
  regs.l = 0;
  succeed(regs);
}

void EsxDos::getSetDrv(Z80::Registers& regs) {
  // '*' asks for the current drive without changing it; anything else is a
  // request to switch, and there is only one drive to switch to.
  regs.a = u8(DriveLetter);
  succeed(regs);
}

void EsxDos::getHandle(Z80::Registers& regs) {
  // A program that has not redirected its output writes to the console, which
  // esxDOS numbers as handle 0.
  regs.a = 0;
  succeed(regs);
}

void EsxDos::getDate(Z80::Registers& regs) {
  // Fixed date: the guest cannot observe real time anyway, and a deterministic
  // answer keeps replays identical.
  regs.a = 4; // Thursday
  setBc(regs, 0); // year, counted from 1980
  regs.d = 0;
  regs.e = 0;
  regs.h = 0;
  regs.l = 0;
  succeed(regs);
}

void EsxDos::open(Bus& bus, Z80::Registers& regs) {
  if (root_.empty()) {
    fail(regs, ErrAcces);
    return;
  }
  const std::string name = readString(bus, regs.ix);
  if (name.empty()) {
    fail(regs, ErrNoEnt);
    return;
  }
  if (name.size() >= MaxPathLength) {
    fail(regs, ErrNameTooLong);
    return;
  }
  const std::string path = resolve(name);
  if (path.empty()) {
    fail(regs, ErrNoEnt);
    return;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    fail(regs, ErrIsDir);
    return;
  }

  const u8 mode = regs.b;
  const bool wantsWrite = (mode & ModeWrite) != 0;
  const bool truncate = (mode & ModeTruncate) == ModeTruncate;
  const bool create = (mode & ModeCreate) != 0;

  const char* hostMode = nullptr;
  if (wantsWrite) hostMode = truncate ? "w+b" : (create ? "w+b" : "r+b");
  else hostMode = "rb";

  std::FILE* file = std::fopen(path.c_str(), hostMode);
  if (file == nullptr && wantsWrite && !create && !truncate)
    file = std::fopen(path.c_str(), "w+b");
  if (file == nullptr) {
    fail(regs, ec ? ErrIO : ErrNoEnt);
    return;
  }

  const u8 number = allocate(file, wantsWrite);
  if (number == 0xFF) {
    std::fclose(file);
    fail(regs, 12 /* esx_enfile */);
    return;
  }

  // Mode 0x40 asks for the first eight bytes of the file in the buffer at DE,
  // which is how a loader recognises what it opened. The position stays at the
  // start of the file afterwards.
  const u16 headerAddress = deOf(regs);
  if ((mode & 0x40) != 0 && headerAddress != 0) {
    u8 header[8] = {};
    if (std::fread(header, 1, sizeof header, file) == 0) {
      std::fclose(file);
      handles_[number] = Entry{};
      fail(regs, ErrIO);
      return;
    }
    for (int i = 0; i < 8; ++i) bus.memWrite(u16(headerAddress + u16(i)), header[i], 0);
    std::rewind(file);
  }

  regs.a = number;
  succeed(regs);
}

void EsxDos::close(Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    fail(regs, ErrBadF);
    return;
  }
  std::fclose(entry->file);
  *entry = Entry{};
  succeed(regs);
}

void EsxDos::read(Bus& bus, Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    setBc(regs, 0);
    fail(regs, ErrBadF);
    return;
  }
  // esxDOS passes the buffer in IX, not HL: callers do `PUSH HL / POP IX`
  // before every `RST 8` (see the loader at 0xAAA8 in ANP2).
  const u16 target = regs.ix;
  const size_t wanted = u16(u16(regs.b) << 8 | regs.c);
  std::array<u8, 4096> chunk{};
  size_t done = 0;
  while (done < wanted) {
    const size_t step = std::min(chunk.size(), wanted - done);
    const size_t got = std::fread(chunk.data(), 1, step, entry->file);
    for (size_t i = 0; i < got; ++i)
      bus.memWrite(u16(target + u16(done + i)), chunk[i], 0);
    done += got;
    if (got < step) break; // short read: end of file or an error
  }
  setBc(regs, u16(done));
  succeed(regs);
}

void EsxDos::write(Bus& bus, Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    setBc(regs, 0);
    fail(regs, ErrBadF);
    return;
  }
  if (!entry->writable) {
    setBc(regs, 0);
    fail(regs, ErrAcces);
    return;
  }
  const u16 source = regs.ix;
  const size_t wanted = u16(u16(regs.b) << 8 | regs.c);
  std::array<u8, 4096> chunk{};
  size_t done = 0;
  while (done < wanted) {
    const size_t step = std::min(chunk.size(), wanted - done);
    for (size_t i = 0; i < step; ++i)
      chunk[i] = bus.memRead(u16(source + u16(done + i)), 0);
    const size_t put = std::fwrite(chunk.data(), 1, step, entry->file);
    done += put;
    if (put < step) break;
  }
  setBc(regs, u16(done));
  succeed(regs);
}

void EsxDos::seek(Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    fail(regs, ErrBadF);
    return;
  }
  // The offset is a signed 32-bit value with the high half in BC, and the seek
  // origin arrives in L rather than in a documented register.
  const i32 offset = i32((u32(regs.b) << 24) | (u32(regs.c) << 16) |
                         (u32(regs.d) << 8) | u32(regs.e));
  int origin = SEEK_SET;
  if (regs.l == 1) origin = SEEK_CUR;
  else if (regs.l == 2) origin = SEEK_END;
  if (std::fseek(entry->file, long(offset), origin) != 0) {
    fail(regs, ErrIO);
    return;
  }
  succeed(regs);
}

void EsxDos::getPosition(Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    fail(regs, ErrBadF);
    return;
  }
  const long position = std::ftell(entry->file);
  if (position < 0) {
    fail(regs, ErrIO);
    return;
  }
  const u32 value = u32(position);
  regs.b = u8(value >> 24);
  regs.c = u8(value >> 16);
  regs.d = u8(value >> 8);
  regs.e = u8(value);
  succeed(regs);
}

void EsxDos::stat(Bus& bus, Z80::Registers& regs) {
  Entry* entry = handle(regs.a);
  if (entry == nullptr) {
    fail(regs, ErrBadF);
    return;
  }
  const long here = std::ftell(entry->file);
  if (here < 0 || std::fseek(entry->file, 0, SEEK_END) != 0) {
    fail(regs, ErrIO);
    return;
  }
  const long size = std::ftell(entry->file);
  std::fseek(entry->file, here, SEEK_SET);
  if (size < 0) {
    fail(regs, ErrIO);
    return;
  }

  // The twelve-byte buffer is the one esxDOS documents: drive, device,
  // attributes, time, date, then the 32-bit size at offset 7. Programs read the
  // size from there, so the layout has to be exact.
  const u16 base = regs.ix;
  const u32 length = u32(size);
  bus.memWrite(base, u8(DriveLetter), 0);
  bus.memWrite(u16(base + 1), 0, 0);
  bus.memWrite(u16(base + 2), AttrArchive, 0);
  writeWord(bus, u16(base + 3), 0);            // time
  writeWord(bus, u16(base + 5), 0x0021);       // 1980-01-01
  writeWord(bus, u16(base + 7), u16(length & 0xFFFF));
  writeWord(bus, u16(base + 9), u16(length >> 16));
  succeed(regs);
}

} // namespace rainbow
