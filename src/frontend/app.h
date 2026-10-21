#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "core/machine.h"
#include "frontend/audio.h"
#include "frontend/input.h"
#include "frontend/ui.h"

namespace mynext::frontend {

class App {
public:
  App() = default;
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Every path is optional. When `rom` is empty the usual candidate locations
  // are probed and the emulator still starts (with a warning) if none are found.
  struct Options {
    std::string rom;
    std::string tape;
    std::string snapshot;
    std::string disk;
  };

  bool init(const Options& options);
  void run();
  void shutdown();

private:
  void handleEvent(const SDL_Event& event);
  void drawToolbar();
  void drawStatusOverlay();
  void drawScreen();
  void updateLayout();
  void computeToolbarMetrics();
  void toggleFullscreen();
  void updateTexture();
  void pushAudio();

  void setMachine(MachineType type);
  enum class DialogKind { Rom, Snapshot, Tape, Disk };

  bool loadRom(const std::string& path);
  bool loadSnapshot(const std::string& path);
  bool loadTape(const std::string& path);
  bool loadDisk(const std::string& path);
  void openFileDialog(DialogKind kind);
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
  DialogKind dialogKind_ = DialogKind::Rom;

  ui::Fonts fonts_;
  float framebufferScale_ = 1.0f;
  float toolbarRowHeight_ = 0.0f;
  float toolbarHeight_ = 0.0f;
  // Screen rectangle in logical points, recomputed every frame to letterbox
  // the 320x256 picture into whatever space the toolbar leaves over.
  ImVec2 screenMin_{};
  ImVec2 screenMax_{};

  bool running_ = true;
  bool paused_ = false;
  bool showDebugger_ = false;
  bool showAbout_ = false;
  bool fullscreen_ = false;
  bool toolbarPinned_ = true;
  bool toolbarVisible_ = true;
  u64 toolbarRevealUntilNs_ = 0;
  int speedMultiplier_ = 1;
  std::string status_;
};

} // namespace mynext::frontend
