#include "peripherals/printer.h"

namespace mynext {

namespace {
constexpr int BytesPerRow = ZxPrinter::Width / 8;
}

ZxPrinter::ZxPrinter() {
  info_.id = "printer";
  info_.name = "ZX Printer";
  info_.version = "1.0";
  info_.author = "myNext";
  info_.description =
      "The Sinclair spark printer on port 0xFB. Captures the dots and can save "
      "the printout as a PBM image.";
  info_.ports = {0xFB};
  rows_.assign(BytesPerRow * 64, 0);
}

void ZxPrinter::reset() {
  motor_ = false;
  strobe_ = false;
}

void ZxPrinter::clear() {
  rows_.assign(BytesPerRow * 64, 0);
  height_ = 0;
  headX_ = 0;
}

bool ZxPrinter::ioWrite(u16 port, u8 value, u32) {
  if ((port & 0x00FF) != 0xFB) return false;

  // Bit 2 is the motor, bit 1 the strobe and bit 7 the dot. The printer watches
  // the port but never claims it, so the write still reaches the ULA path.
  motor_ = (value & 0x04) != 0;
  const bool strobe = (value & 0x02) != 0;
  if (strobe || !strobe_) {
    strobe_ = strobe;
    return false;
  }
  strobe_ = false;

  if (headX_ == 0) newLine();
  setPixel(headX_, height_ - 1, (value & 0x80) != 0);
  ++headX_;
  if (headX_ >= Width) headX_ = 0;
  return false;
}

void ZxPrinter::newLine() {
  if (height_ >= MaxHeight) {
    // Drop the oldest line so a long session cannot grow without bound.
    rows_.erase(rows_.begin(), rows_.begin() + BytesPerRow);
    --height_;
  }
  rows_.insert(rows_.end(), BytesPerRow, 0);
  ++height_;
}

void ZxPrinter::setPixel(int x, int y, bool on) {
  if (x < 0 || x >= Width || y < 0 || y >= height_) return;
  const size_t offset = size_t(y) * BytesPerRow + size_t(x / 8);
  const u8 mask = u8(0x80 >> (x & 7));
  if (on) {
    rows_[offset] |= mask;
  } else {
    rows_[offset] &= u8(~mask);
  }
}

bool ZxPrinter::pixel(int x, int y) const {
  if (x < 0 || x >= Width || y < 0 || y >= height_) return false;
  return (rows_[size_t(y) * BytesPerRow + size_t(x / 8)] & (0x80 >> (x & 7))) != 0;
}

std::string ZxPrinter::toPbm() const {
  std::string out = "P1\n# myNext ZX Printer\n256 " + std::to_string(height_) + "\n";
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < Width; ++x) {
      out += pixel(x, y) ? '1' : '0';
      if (x + 1 < Width) out += ' ';
    }
    out += '\n';
  }
  return out;
}

} // namespace mynext
