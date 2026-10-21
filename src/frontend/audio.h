#pragma once

#include <vector>

#include <SDL3/SDL.h>

namespace mynext::frontend {

// Thin wrapper around an SDL3 audio stream. The core produces mono float
// samples; SDL handles device format conversion and buffering.
class Audio {
public:
  bool init(int sampleRate);
  void shutdown();
  void push(const std::vector<float>& samples);
  int sampleRate() const { return spec_.freq; }

private:
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioSpec spec_{};
};

} // namespace mynext::frontend
