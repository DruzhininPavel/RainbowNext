#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/machine.h"
#include "frontend/audio.h"
#include "frontend/config.h"
#include "frontend/debugger.h"
#include "frontend/input.h"
#include "frontend/ui.h"

namespace rainbow::frontend {

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
    std::string sd;
    bool debugger = false;  // start paused with the debugger open
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
  enum class DialogKind {
    Rom,
    Snapshot,
    Tape,
    Disk,
    Cartridge,
    DivMmcRom,
    DivMmcCard,
    Printout
  };

  bool loadRom(const std::string& path);
  bool loadSnapshot(const std::string& path);
  bool loadTape(const std::string& path);
  bool loadDisk(const std::string& path);
  bool loadCartridge(const std::string& path);
  bool loadDivMmcRom(const std::string& path);
  bool loadDivMmcCard(const std::string& path);
  // The card image is kept here, not in the module, so switching the machine
  // type -- which rebuilds the bus -- does not silently take the card out.
  void insertSdCard();
  void ejectSdCard();
  bool savePrintout(const std::string& path);
  void openFileDialog(DialogKind kind);
  void openSaveDialog(DialogKind kind);
  void reset();

  // Settings that outlive a run: which peripherals are plugged in and where
  // plugin libraries are looked for.
  void loadConfig();
  void saveConfig() const;
  void syncConfigFromBus();
  void applyPeripheralConfig();
  // Re-reads the plugin folder and swaps the external modules in the bus.
  void scanPeripherals();
  void drawPeripheralsMenu();
  void feedMouse();

  static void SDLCALL onFileDialog(void* userdata, const char* const* filelist, int filter);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  bool imguiReady_ = false;

  std::unique_ptr<Machine> machine_;
  Audio audio_;
  Input input_;

  std::string romPath_;
  std::string sdPath_;
  std::vector<u8> sdImage_;
  std::string pendingDialogResult_;
  bool dialogOpen_ = false;
  bool dialogSave_ = false;
  DialogKind dialogKind_ = DialogKind::Rom;

  Config config_;
  std::string configPath_;
  std::string pluginDir_;
  std::vector<std::string> pluginErrors_;
  // Ids of the modules that came from a library, so a rescan can take them back
  // out without touching the built-in ones.
  std::vector<std::string> pluginIds_;

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
  DebuggerUi debuggerUi_;
  bool showDebugger_ = false;
  bool showAbout_ = false;
  bool fullscreen_ = false;
  bool toolbarPinned_ = true;
  bool toolbarVisible_ = true;
  u64 toolbarRevealUntilNs_ = 0;
  int speedMultiplier_ = 1;
  std::string status_;
};

} // namespace rainbow::frontend
