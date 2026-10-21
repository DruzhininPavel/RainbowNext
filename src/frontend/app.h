#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "core/machine.h"
#include "frontend/audio.h"
#include "frontend/input.h"

namespace mynext::frontend {

class App {
public:
  App() = default;
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // `romPath` is optional; when it is empty the usual candidate locations are
  // probed and the emulator still starts (with a warning) if none are found.
  bool init(const std::string& romPath);
  void run();
  void shutdown();

private:
  void handleEvent(const SDL_Event& event);
  void newFrame();
  void render();
  void drawMenuBar();
  void drawStatusOverlay();
  void updateTexture();
  void pushAudio();

  void setMachine(MachineType type);
  bool loadRom(const std::string& path);
  bool loadSnapshot(const std::string& path);
  void openFileDialog(bool snapshot);
  void reset();

  static void SDLCALL onFileDialog(void* userdata, const char* const* filelist, int filter);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  bool imguiReady_ = false;

  std::unique_ptr<Machine> machine_;
  Audio audio_;
  Input input_;

  std::string romPath_;
  std::string pendingDialogResult_;
  bool dialogOpen_ = false;
  bool dialogForSnapshot_ = false;

  bool running_ = true;
  bool paused_ = false;
  bool showDebugger_ = false;
  bool showAbout_ = false;
  int speedMultiplier_ = 1;
  std::string status_;
};

} // namespace mynext::frontend
