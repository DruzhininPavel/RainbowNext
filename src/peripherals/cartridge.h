#pragma once

#include <string>
#include <vector>

#include "core/peripheral.h"

namespace mynext {

// A ROM cartridge in the spirit of the ZX Interface 2: the image takes over the
// bottom 16K of the address space while the module is plugged in, and the
// machine's own ROM is ignored. Images are padded to a whole number of 16K
// pages, so a 16K, 32K or 48K cartridge all work.
class Cartridge : public Peripheral {
public:
  static constexpr size_t PageSize = 0x4000;

  Cartridge();

  const PeripheralInfo& info() const override { return info_; }
  const std::vector<u8>* rom() const override { return image_.empty() ? nullptr : &image_; }

  void reset() override {}
  const char* fileFilter() const override { return "Cartridge (*.rom;*.bin;*.cart)"; }
  bool loadFile(const std::string& path, std::string& error) override;
  bool loadData(const u8* data, size_t size, std::string& error);
  void eject() { image_.clear(); }
  bool loaded() const { return !image_.empty(); }
  size_t size() const { return image_.size(); }

private:
  PeripheralInfo info_;
  std::vector<u8> image_;
};

} // namespace mynext
