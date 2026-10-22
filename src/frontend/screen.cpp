#include "frontend/screen.h"

#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

#include "frontend/shaders/crt_shaders.h"

namespace rainbow::frontend {
namespace {

// Mirrors the UBO in crt.vert / crt.frag. Kept as plain floats so it can be
// pushed straight into the command buffer.
struct CrtUniforms {
  float dest[4];    // x, y, width, height of the picture, in output pixels
  float surface[4]; // output width, output height, source width, source height
  float params[4];  // scanline strength, mask strength, crt enabled, unused
};

constexpr int kUniformFloats = sizeof(CrtUniforms) / sizeof(float);

SDL_GPUShaderFormat pickShaderFormat(SDL_GPUDevice* device, const void** vert, size_t* vertSize,
                                     const void** frag, size_t* fragSize) {
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
    *vert = shaders::kCrtVertMsl;
    *vertSize = sizeof(shaders::kCrtVertMsl) - 1; // drop the trailing NUL
    *frag = shaders::kCrtFragMsl;
    *fragSize = sizeof(shaders::kCrtFragMsl) - 1;
    return SDL_GPU_SHADERFORMAT_MSL;
  }
  if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
    *vert = shaders::kCrtVertSpirv;
    *vertSize = sizeof(shaders::kCrtVertSpirv);
    *frag = shaders::kCrtFragSpirv;
    *fragSize = sizeof(shaders::kCrtFragSpirv);
    return SDL_GPU_SHADERFORMAT_SPIRV;
  }
  return SDL_GPU_SHADERFORMAT_INVALID;
}

SDL_GPUShader* makeShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage, const void* code,
                          size_t codeSize, SDL_GPUShaderFormat format, Uint32 uniformBuffers,
                          Uint32 samplers) {
  SDL_GPUShaderCreateInfo info{};
  info.code = static_cast<const Uint8*>(code);
  info.code_size = codeSize;
  info.entrypoint = nullptr; // SDL defaults to "main" (SPIR-V) / "main0" (MSL)
  info.format = format;
  info.stage = stage;
  info.num_uniform_buffers = uniformBuffers;
  info.num_samplers = samplers;
  return SDL_CreateGPUShader(device, &info);
}

} // namespace

ScreenRenderer::~ScreenRenderer() { shutdown(); }

bool ScreenRenderer::init(SDL_Window* window, int width, int height) {
  window_ = window;
  width_ = width;
  height_ = height;

  // Both formats are requested so SDL can pick Metal on macOS and Vulkan
  // everywhere else without us guessing.
  device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
#ifndef NDEBUG
                                true,
#else
                                false,
#endif
                                nullptr);
  if (device_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
    return false;
  }
  if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
    std::fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
    return false;
  }
  // The emulator paces its own frames, so the swapchain must not block on the
  // display.
  SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                SDL_GPU_PRESENTMODE_IMMEDIATE);
  swapchainFormat_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);

  const void* vertCode = nullptr;
  const void* fragCode = nullptr;
  size_t vertSize = 0;
  size_t fragSize = 0;
  const SDL_GPUShaderFormat format =
      pickShaderFormat(device_, &vertCode, &vertSize, &fragCode, &fragSize);
  if (format == SDL_GPU_SHADERFORMAT_INVALID) {
    std::fprintf(stderr, "No usable GPU shader format (need MSL or SPIR-V)\n");
    return false;
  }

  SDL_GPUShader* vert =
      makeShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, vertCode, vertSize, format, 1, 0);
  SDL_GPUShader* frag =
      makeShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fragCode, fragSize, format, 1, 1);
  if (vert == nullptr || frag == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUShader failed: %s\n", SDL_GetError());
    SDL_ReleaseGPUShader(device_, vert);
    SDL_ReleaseGPUShader(device_, frag);
    return false;
  }

  SDL_GPUColorTargetDescription colorTarget{};
  colorTarget.format = swapchainFormat_;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = vert;
  info.fragment_shader = frag;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
  info.target_info.color_target_descriptions = &colorTarget;
  info.target_info.num_color_targets = 1;
  pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
  SDL_ReleaseGPUShader(device_, vert);
  SDL_ReleaseGPUShader(device_, frag);
  if (pipeline_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());
    return false;
  }

  SDL_GPUSamplerCreateInfo samplerInfo{};
  samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
  samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
  if (sampler_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUSampler failed: %s\n", SDL_GetError());
    return false;
  }

  // The machine hands us 0xAARRGGBB words, which on disk are B,G,R,A bytes.
  SDL_GPUTextureCreateInfo textureInfo{};
  textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
  textureInfo.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
  textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  textureInfo.width = static_cast<Uint32>(width_);
  textureInfo.height = static_cast<Uint32>(height_);
  textureInfo.layer_count_or_depth = 1;
  textureInfo.num_levels = 1;
  texture_ = SDL_CreateGPUTexture(device_, &textureInfo);
  if (texture_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUTexture failed: %s\n", SDL_GetError());
    return false;
  }

  SDL_GPUTransferBufferCreateInfo transferInfo{};
  transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transferInfo.size = static_cast<Uint32>(width_ * height_ * 4);
  transfer_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
  if (transfer_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

void ScreenRenderer::shutdown() {
  if (device_ != nullptr) {
    SDL_WaitForGPUIdle(device_);
    if (transfer_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    if (texture_ != nullptr) SDL_ReleaseGPUTexture(device_, texture_);
    if (sampler_ != nullptr) SDL_ReleaseGPUSampler(device_, sampler_);
    if (pipeline_ != nullptr) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (window_ != nullptr) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
  }
  transfer_ = nullptr;
  texture_ = nullptr;
  sampler_ = nullptr;
  pipeline_ = nullptr;
  device_ = nullptr;
  window_ = nullptr;
  swapchain_ = nullptr;
}

SDL_GPUCommandBuffer* ScreenRenderer::beginFrame(const void* pixels, int pitch) {
  if (device_ == nullptr) return nullptr;

  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  if (command == nullptr) return nullptr;

  if (!SDL_AcquireGPUSwapchainTexture(command, window_, &swapchain_, &swapchainWidth_,
                                      &swapchainHeight_) ||
      swapchain_ == nullptr) {
    // Nothing to draw into (minimised). The command buffer still has to be
    // submitted or it leaks.
    SDL_SubmitGPUCommandBuffer(command);
    swapchain_ = nullptr;
    return nullptr;
  }

  const Uint32 bytesPerRow = static_cast<Uint32>(width_) * 4;
  void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true);
  if (mapped == nullptr) {
    SDL_SubmitGPUCommandBuffer(command);
    return nullptr;
  }
  const auto* rows = static_cast<const Uint8*>(pixels);
  auto* target = static_cast<Uint8*>(mapped);
  for (int y = 0; y < height_; ++y) {
    std::memcpy(target + size_t(y) * bytesPerRow, rows + size_t(y) * size_t(pitch), bytesPerRow);
  }
  SDL_UnmapGPUTransferBuffer(device_, transfer_);

  SDL_GPUTextureTransferInfo source{};
  source.transfer_buffer = transfer_;
  SDL_GPUTextureRegion region{};
  region.texture = texture_;
  region.w = static_cast<Uint32>(width_);
  region.h = static_cast<Uint32>(height_);
  region.d = 1;

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  SDL_UploadToGPUTexture(copy, &source, &region, true);
  SDL_EndGPUCopyPass(copy);
  return command;
}

void ScreenRenderer::endFrame(SDL_GPUCommandBuffer* command, ImDrawData* drawData, float minX,
                              float minY, float maxX, float maxY, float scale,
                              const CrtSettings& crt) {
  if (command == nullptr) return;

  CrtUniforms uniforms{};
  uniforms.dest[0] = minX * scale;
  uniforms.dest[1] = minY * scale;
  uniforms.dest[2] = (maxX - minX) * scale;
  uniforms.dest[3] = (maxY - minY) * scale;
  uniforms.surface[0] = static_cast<float>(swapchainWidth_);
  uniforms.surface[1] = static_cast<float>(swapchainHeight_);
  uniforms.surface[2] = static_cast<float>(width_);
  uniforms.surface[3] = static_cast<float>(height_);
  uniforms.params[0] = crt.scanlines;
  uniforms.params[1] = crt.mask;
  uniforms.params[2] = crt.enabled ? 1.0f : 0.0f;

  SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));
  SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
  static_assert(kUniformFloats == 12, "uniform block must stay 48 bytes");

  SDL_GPUColorTargetInfo colorTarget{};
  colorTarget.texture = swapchain_;
  colorTarget.clear_color = {6.0f / 255.0f, 6.0f / 255.0f, 9.0f / 255.0f, 1.0f};
  colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
  colorTarget.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &colorTarget, 1, nullptr);

  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_GPUTextureSamplerBinding binding{};
  binding.texture = texture_;
  binding.sampler = sampler_;
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

  ImGui_ImplSDLGPU3_RenderDrawData(drawData, command, pass);
  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(command);
  swapchain_ = nullptr;
}

} // namespace rainbow::frontend
