#pragma once

#include <SDL3/SDL.h>

struct ImDrawData;

namespace rainbow::frontend {

// Strength of the optional CRT pass. Both values are 0..1 and are applied by
// the fragment shader; `enabled == false` takes the plain nearest-neighbour
// path with no extra maths per pixel.
struct CrtSettings {
  bool enabled = true;
  float scanlines = 0.35f;
  float mask = 0.45f;
};

// Draws the emulator picture and the ImGui overlay through SDL_GPU.
//
// The picture is uploaded into a 320x256 texture and drawn as a single triangle
// whose vertex shader positions it over the letterboxed destination rectangle,
// so the CRT pass costs one full-screen quad and nothing more.
class ScreenRenderer {
public:
  ScreenRenderer() = default;
  ~ScreenRenderer();

  ScreenRenderer(const ScreenRenderer&) = delete;
  ScreenRenderer& operator=(const ScreenRenderer&) = delete;

  bool init(SDL_Window* window, int width, int height);
  void shutdown();

  SDL_GPUDevice* device() const { return device_; }
  SDL_GPUTextureFormat swapchainFormat() const { return swapchainFormat_; }

  // Acquires a command buffer and the swapchain and uploads `pixels`. Returns
  // nullptr when the window has no drawable area this frame (minimised), in
  // which case the caller must skip rendering entirely.
  SDL_GPUCommandBuffer* beginFrame(const void* pixels, int pitch);

  // Draws the picture and then `drawData` on top, and submits the frame.
  // `scale` converts the logical-point rectangle into the physical pixels the
  // swapchain is measured in.
  void endFrame(SDL_GPUCommandBuffer* command, ImDrawData* drawData, float minX, float minY,
                float maxX, float maxY, float scale, const CrtSettings& crt);

private:
  SDL_Window* window_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  SDL_GPUTransferBuffer* transfer_ = nullptr;
  SDL_GPUTextureFormat swapchainFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* swapchain_ = nullptr;
  Uint32 swapchainWidth_ = 0;
  Uint32 swapchainHeight_ = 0;
  int width_ = 0;
  int height_ = 0;
};

} // namespace rainbow::frontend
