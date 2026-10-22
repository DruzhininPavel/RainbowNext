#include "peripherals/cartridge.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace rainbow {

Cartridge::Cartridge() {
  info_.id = "cartridge";
  info_.name = "ROM Cartridge";
  info_.version = "1.0";
  info_.author = "RainbowNext";
  info_.description =
      "Maps a 16K ROM image at 0x0000 and hides the machine's own ROM, like a "
      "ZX Interface 2 cartridge. Load a .rom or .bin image from the menu.";
}

bool Cartridge::loadFile(const std::string& path, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  const std::vector<u8> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  return loadData(data.data(), data.size(), error);
}

bool Cartridge::loadData(const u8* data, size_t size, std::string& error) {
  if (data == nullptr || size == 0) {
    error = "the cartridge image is empty";
    return false;
  }
  if (size > PageSize * 4) {
    error = "the cartridge image is larger than 64K";
    return false;
  }
  image_.assign(size, 0);
  std::copy(data, data + size, image_.begin());
  // Pad to a whole number of 16K pages so the address decode is a mask.
  const size_t padded = ((size + PageSize - 1) / PageSize) * PageSize;
  image_.resize(padded, 0xFF);
  return true;
}

} // namespace rainbow
