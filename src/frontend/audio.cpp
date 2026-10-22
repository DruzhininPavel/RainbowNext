#include "frontend/audio.h"

#include <cstdio>

namespace rainbow::frontend {

bool Audio::init(int sampleRate) {
  spec_.format = SDL_AUDIO_F32;
  spec_.channels = 2;
  spec_.freq = sampleRate;

  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec_, nullptr, nullptr);
  if (stream_ == nullptr) {
    std::fprintf(stderr, "audio: %s\n", SDL_GetError());
    return false;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void Audio::shutdown() {
  if (stream_ != nullptr) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void Audio::push(const std::vector<float>& samples) {
  if (stream_ == nullptr || samples.empty()) return;

  // Keep latency bounded: one second of stereo float audio is `freq * 8` bytes.
  const int limit = spec_.freq * 8;
  if (SDL_GetAudioStreamQueued(stream_) > limit) return;

  SDL_PutAudioStreamData(stream_, samples.data(), int(samples.size() * sizeof(float)));
}

} // namespace rainbow::frontend
