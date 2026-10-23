#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/machine.h"
#include "core/rewind.h"
#include "core/rzx_session.h"
#include "frontend/audio.h"
#include "frontend/autoload.h"
#include "frontend/config.h"
#include "frontend/debugger.h"
#include "frontend/input.h"
#include "frontend/screen.h"
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
    std::string machine;  // short id: 48, 128, plus2, plus3, pentagon, next
    std::string tape;
    std::string snapshot;
    std::string disk;
    std::string sd;
    std::string esxdos;     // host directory mounted as drive C for RST 8 calls
    std::string state;      // save state to restore at startup
    std::string rzx;        // RZX replay to play back at startup
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
  void revealToolbar();
  void pushAudio();

  void setMachine(MachineType type);
  enum class DialogKind {
    Rom,
    Snapshot,
    Tape,
    Disk,
    Cartridge,
    SdCard,
    Printout,
    StateSave,
    StateLoad,
    RzxSave,
    RzxLoad
  };

  bool loadRom(const std::string& path);
  bool loadSnapshot(const std::string& path);  bool loadTape(const std::string& path);
  bool loadDisk(const std::string& path);
  bool loadCartridge(const std::string& path);
  // `persist` is false during startup, where the config is written once at the
  // end; the choice is still recorded in memory either way.
  bool loadSdCard(const std::string& path, bool persist = true);
  bool mountEsxDos(const std::string& path);
  // The card image is kept here as well as in the machine, so switching the
  // machine type -- which rebuilds the bus -- does not silently take the card
  // out, and so the window can show the path it came from.
  void ejectSdCard();
  bool savePrintout(const std::string& path);
  bool saveStateFile(const std::string& path);
  bool loadStateFile(const std::string& path);
  // RZX replay: the recording is made from the running machine and only named
  // once it is stopped, so starting one needs no path.
  void startRzxRecording();
  bool saveRzxFile(const std::string& path);
  bool loadRzxFile(const std::string& path);
  void stopRzx();
  // F8/F9 use one well-known file instead of asking for a name every time.
  void quickSaveState();
  void quickLoadState();
  void rewindOneFrame();
  void clearRewind();
  void openFileDialog(DialogKind kind);
  void openSaveDialog(DialogKind kind);
  // The ROM dialogs remember which machine they were opened for: File > Open
  // ROM targets the running machine, the ROM window targets whichever row was
  // clicked and only applies at once if that machine is the one running.
  void openRomDialog(DialogKind kind, MachineType target);
  void reset();
  // Types the ROM's load command and starts the deck; see Autoload.
  void autoloadTape();

  // ROM images. Each machine remembers the files chosen for it, so a second
  // start needs no clicking; an empty choice falls back to the roms/ lookup.
  const std::string& romChoice(MachineType type) const;
  // `persist` is false only during startup, where the config is written once at
  // the end: saving earlier would record the machine's default peripherals
  // before the user's list has been replayed.
  void setRomChoice(MachineType type, const std::string& path, bool persist = true);
  // Loads the remembered (or discovered) ROM for `type` into machine_.
  bool applyRomChoice(MachineType type);
  // Where a machine's ROM would come from right now, for the ROM window.
  std::string effectiveRomPath(MachineType type) const;

  // The card a machine boots from. Kept next to the ROM for the same reason:
  // the Next is not usable without one, and picking it by hand every start is
  // what this avoids.
  const std::string& sdChoice(MachineType type) const;
  void setSdChoice(MachineType type, const std::string& path, bool persist = true);
  // Puts the remembered card into the running machine, if there is one.
  bool applySdChoice(MachineType type);
  void drawRomsWindow();

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
  ScreenRenderer screen_;
  bool imguiReady_ = false;
  CrtSettings crt_;

  std::unique_ptr<Machine> machine_;
  Audio audio_;
  Input input_;

  std::string romPath_;
  std::string divMmcPath_;
  std::optional<MachineType> romDialogTarget_;
  bool showRoms_ = false;
  std::string quickStatePath_;  std::string sdPath_;
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
  RewindBuffer rewind_;
  bool rewindEnabled_ = true;
  int rewindStride_ = 0;
  // Declared after machine_ so it detaches from the machine before the machine
  // is destroyed.
  RzxSession rzx_;
  Autoload autoload_;
  std::string status_;
};

} // namespace rainbow::frontend
