#include "core/rewind.h"

#include <utility>

namespace rainbow {

void RewindBuffer::push(std::vector<u8> state) {
  if (state.size() > maxBytes_) return;
  while (!states_.empty() && bytes_ + state.size() > maxBytes_) {
    bytes_ -= states_.front().size();
    states_.pop_front();
  }
  bytes_ += state.size();
  states_.push_back(std::move(state));
}

bool RewindBuffer::pop(std::vector<u8>& out) {
  if (states_.empty()) return false;
  out = std::move(states_.back());
  states_.pop_back();
  bytes_ -= out.size();
  return true;
}

void RewindBuffer::clear() {
  states_.clear();
  bytes_ = 0;
}

} // namespace rainbow
