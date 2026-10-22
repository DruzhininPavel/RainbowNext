#pragma once

#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "core/types.h"

namespace rainbow {

// A save state is a little-endian byte stream. Every subsystem implements one
// `serialize` template that both writes and reads, so a field can never be
// saved by one direction and skipped by the other; the machine wraps the stream
// in a magic word, a version and a checksum, so a state from another build is
// refused instead of being misread.
//
// The two ends share every overload, which is what keeps them in step:
//
//   * integers and enums are written at their own width, little-endian;
//   * `bool` is one byte and `float` is its IEEE bit pattern;
//   * C arrays, `std::array` and `std::vector<u8>` are their elements;
//   * any other class is asked to serialise itself.
//
// Fixed-width fields keep a state portable between arm64 and x86-64; `int`,
// `size_t` and pointers are written at the width of the build that wrote them,
// which is why the version word and the machine type are checked before
// anything is applied.
class StateWriter {
public:
  template <class T> void operator()(T& value) { field(value); }

  template <class T,
            std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  void field(T& value) {
    const u64 bits = u64(value);
    for (size_t i = 0; i < sizeof(T); ++i) bytes_.push_back(u8(bits >> (8 * i)));
  }

  void field(bool& value) { bytes_.push_back(value ? 1 : 0); }

  void field(float& value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    field(bits);
  }

  template <class T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
  void field(T& value) {
    auto raw = static_cast<std::underlying_type_t<T>>(value);
    field(raw);
  }

  template <class T, size_t N> void field(std::array<T, N>& array) {
    for (T& entry : array) field(entry);
  }

  template <class T, size_t N> void field(T (&array)[N]) {
    for (T& entry : array) field(entry);
  }

  void field(std::vector<u8>& bytes) {
    u32 count = u32(bytes.size());
    field(count);
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  template <class T,
            std::enable_if_t<!std::is_same_v<T, u8>, int> = 0>
  void field(std::vector<T>& values) {
    u32 count = u32(values.size());
    field(count);
    for (T& value : values) field(value);
  }

  void raw(const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
  }

  template <class T, std::enable_if_t<std::is_class_v<T>, int> = 0>
  void field(T& value) {
    value.serialize(*this);
  }

  const std::vector<u8>& bytes() const { return bytes_; }
  std::vector<u8> take() { return std::move(bytes_); }
  size_t size() const { return bytes_.size(); }

private:
  std::vector<u8> bytes_;
};

class StateReader {
public:
  StateReader(const u8* data, size_t size) : data_(data), size_(size) {}

  template <class T> void operator()(T& value) { field(value); }

  template <class T,
            std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  void field(T& value) {
    if (!take(sizeof(T))) return;
    u64 bits = 0;
    for (size_t i = 0; i < sizeof(T); ++i) bits |= u64(data_[offset_ + i]) << (8 * i);
    offset_ += sizeof(T);
    value = T(bits);
  }

  void field(bool& value) {
    if (!take(1)) return;
    value = data_[offset_++] != 0;
  }

  void field(float& value) {
    u32 bits = 0;
    field(bits);
    if (!ok()) return;
    std::memcpy(&value, &bits, sizeof bits);
  }

  template <class T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
  void field(T& value) {
    std::underlying_type_t<T> raw{};
    field(raw);
    if (!ok()) return;
    value = static_cast<T>(raw);
  }

  template <class T, size_t N> void field(std::array<T, N>& array) {
    for (T& entry : array) field(entry);
  }

  template <class T, size_t N> void field(T (&array)[N]) {
    for (T& entry : array) field(entry);
  }

  void field(std::vector<u8>& bytes) {
    u32 count = 0;
    field(count);
    if (!take(count)) return;
    bytes.assign(data_ + offset_, data_ + offset_ + count);
    offset_ += count;
  }

  template <class T,
            std::enable_if_t<!std::is_same_v<T, u8>, int> = 0>
  void field(std::vector<T>& values) {
    u32 count = 0;
    field(count);
    if (!ok()) return;
    values.assign(size_t(count), T{});
    for (T& value : values) field(value);
  }

  void raw(void* data, size_t size) {
    if (!take(size)) return;
    std::memcpy(data, data_ + offset_, size);
    offset_ += size;
  }

  template <class T, std::enable_if_t<std::is_class_v<T>, int> = 0>
  void field(T& value) {
    value.serialize(*this);
  }

  bool ok() const { return failed_.empty(); }
  const std::string& error() const { return failed_; }
  // True when every byte was consumed, which is how a state written by a
  // version with an extra field is caught even when the checksum matches.
  bool consumedAll() const { return offset_ == size_; }
  size_t offset() const { return offset_; }
  size_t size() const { return size_; }

  void fail(const std::string& why) {
    if (failed_.empty()) failed_ = why;
  }

private:
  bool take(size_t count) {
    if (!failed_.empty()) return false;
    if (count > size_ - offset_) {
      fail("the state ends in the middle of a value");
      return false;
    }
    return true;
  }

  const u8* data_ = nullptr;
  size_t size_ = 0;
  size_t offset_ = 0;
  std::string failed_;
};

// FNV-1a, used as the trailing checksum of a state. Not a security measure: it
// only catches a truncated or corrupted file before it is applied.
inline u64 stateChecksum(const u8* data, size_t size) {
  u64 hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

} // namespace rainbow
