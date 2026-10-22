#include "frontend/app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "frontend/debugger.h"
#include "frontend/ui.h"
#include "peripherals/cartridge.h"
#include "peripherals/divmmc.h"
#include "peripherals/kempston_mouse.h"
#include "peripherals/printer.h"
#include "plugin/loader.h"

namespace rainbow::frontend {
namespace {

// One rewind state every four frames: often enough to feel continuous when the
// key is held, rare enough that copying memory does not eat the frame budget.
constexpr int kRewindStride = 4;

constexpr MachineType AllMachines[] = {
    MachineType::Spectrum48, MachineType::Spectrum128, MachineType::Plus2,
    MachineType::Plus3,      MachineType::Pentagon,    MachineType::Next,
};

// ROM sets are user supplied (see roms/README.md); these are the base names the
// emulator looks for, in order, when a machine is selected.
std::vector<std::string> romCandidates(MachineType type) {
  switch (type) {
    case MachineType::Spectrum48: return {"48"};
    case MachineType::Spectrum128: return {"128", "128k"};
    case MachineType::Plus2: return {"plus2", "plus2a", "128"};
    case MachineType::Plus3: return {"plus3", "plus2a"};
    case MachineType::Pentagon: return {"pentagon", "128"};
    case MachineType::Next: return {"next", "128"};
  }
  return {"48"};
}

// A ROM set is either a single image (`128.rom`) or one file per 16K bank
// (`128-0.rom`, `128-1.rom`, ...). Both layouts are accepted, and a bank file
// must be exactly 16K so a half-written dump is rejected instead of silently
// shifting every following bank.
bool loadRomSet(Machine& machine, const std::string& base) {
  std::vector<u8> image;
  for (int bank = 0; bank < Machine::RomBanks; ++bank) {
    const std::string path = base + "-" + std::to_string(bank) + ".rom";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) break;
    u8 page[Machine::BankSize];
    const size_t got = std::fread(page, 1, sizeof page, file);
    std::fclose(file);
    if (got != sizeof page) {
      image.clear();
      break;
    }
    image.insert(image.end(), page, page + sizeof page);
  }
  if (!image.empty()) return machine.loadRomImageData(image.data(), image.size());
  return machine.loadRomImage(base + ".rom");
}

// Looks for a ROM set in `dir` for every candidate base name of `type`.
bool loadRomFor(Machine& machine, MachineType type, const std::string& dir, std::string* used) {
  for (const std::string& base : romCandidates(type)) {
    const std::string stem = dir + base;
    if (loadRomSet(machine, stem)) {
      if (used != nullptr) {
        // Report the file that was actually opened, not the base name.
        *used = stem + "-0.rom";
        std::FILE* probe = std::fopen(used->c_str(), "rb");
        if (probe != nullptr) {
          std::fclose(probe);
        } else {
          *used = stem + ".rom";
        }
      }
      return true;
    }
  }
  return false;
}

// A downloaded build keeps `roms/` next to the executable, while a development
// build keeps it in the working directory, so both are searched.
bool loadRomFromAnywhere(Machine& machine, MachineType type, std::string* used) {
  if (loadRomFor(machine, type, "roms/", used)) return true;
  if (const char* base = SDL_GetBasePath()) {
    if (loadRomFor(machine, type, std::string(base) + "roms/", used)) return true;
  }
  return loadRomFor(machine, type, "", used);
}

float chipWidth(const char* text) { return ImGui::CalcTextSize(text).x + ui::kChipPadX * 2.0f; }

void drawChip(ImDrawList* drawList, ImVec2 pos, const char* text, ImU32 background, ImU32 foreground,
              float height) {
  const ImVec2 max(pos.x + chipWidth(text), pos.y + height);
  drawList->AddRectFilled(pos, max, background, 5.0f);
  const float textY = pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
  drawList->AddText(ImVec2(pos.x + ui::kChipPadX, textY), foreground, text);
}

std::string lowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

bool isNexPath(const std::string& path) { return lowerExtension(path) == ".nex"; }

} // namespace

App::~App() { shutdown(); }

void SDLCALL App::onFileDialog(void* userdata, const char* const* filelist, int) {
  auto* app = static_cast<App*>(userdata);
  if (filelist != nullptr && filelist[0] != nullptr) app->pendingDialogResult_ = filelist[0];
  app->dialogOpen_ = false;
}

bool App::init(const Options& options) {
  SDL_SetAppMetadata("RainbowNext", "0.1.0", "com.rainbownext.emulator");

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }

  window_ = SDL_CreateWindow("RainbowNext", Machine::FrameWidth * 3, Machine::FrameHeight * 3,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }

  if (!screen_.init(window_, Machine::FrameWidth, Machine::FrameHeight)) {
    std::fprintf(stderr, "GPU initialisation failed\n");
    return false;
  }

  machine_ = std::make_unique<Machine>(MachineType::Spectrum48);
  rzx_.bind(*machine_);
  audio_.init(44100);
  machine_->setSampleRate(audio_.sampleRate() > 0 ? audio_.sampleRate() : 44100);

  bool loaded = false;
  if (!options.rom.empty() && machine_->loadRomImage(options.rom)) {
    romPath_ = options.rom;
    loaded = true;
  }
  if (!loaded) loaded = loadRomFromAnywhere(*machine_, machine_->type(), &romPath_);
  if (loaded) {
    status_ = "ROM loaded: " + romPath_;
  }
  if (!machine_->romLoaded()) {
    status_ = "No ROM loaded - put 48.rom into roms/ or use File > Open ROM";
  }

  if (!options.snapshot.empty()) loadSnapshot(options.snapshot);
  if (!options.tape.empty()) loadTape(options.tape);
  if (!options.disk.empty()) loadDisk(options.disk);

  input_.openGamepads();

  loadConfig();
  scanPeripherals();
  applyPeripheralConfig();
  if (!options.sd.empty()) loadDivMmcCard(options.sd);
  if (!options.state.empty()) loadStateFile(options.state);
  if (!options.rzx.empty()) loadRzxFile(options.rzx);
  if (options.debugger) {
    showDebugger_ = true;
    paused_ = true;
  }
  // Write the file on the first run too, so the plugin folder is recorded even
  // if the emulator is killed rather than closed.
  saveConfig();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ui::applyTheme(ImGui::GetStyle());
  fonts_ = ui::loadFonts();
  if (!ImGui_ImplSDL3_InitForSDLGPU(window_)) {
    std::fprintf(stderr, "ImGui SDL3 backend failed: %s\n", SDL_GetError());
    return false;
  }
  ImGui_ImplSDLGPU3_InitInfo gpuInfo{};
  gpuInfo.Device = screen_.device();
  gpuInfo.ColorTargetFormat = screen_.swapchainFormat();
  gpuInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  if (!ImGui_ImplSDLGPU3_Init(&gpuInfo)) {
    std::fprintf(stderr, "ImGui SDL_GPU backend failed: %s\n", SDL_GetError());
    return false;
  }
  imguiReady_ = true;
  return true;
}

void App::shutdown() {
  saveConfig();
  input_.closeGamepads();
  audio_.shutdown();

  if (imguiReady_) {
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    imguiReady_ = false;
  }
  screen_.shutdown();
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  SDL_Quit();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::run() {
  const u64 baseFrameNs =
      1'000'000'000ull * u64(machine_->tstatesPerFrameValue()) / u64(machine_->timings().cpuHz);
  u64 next = SDL_GetTicksNS();

  while (running_) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) handleEvent(event);

    if (!pendingDialogResult_.empty()) {
      const std::string path = pendingDialogResult_;
      pendingDialogResult_.clear();
      switch (dialogKind_) {
        case DialogKind::Snapshot: loadSnapshot(path); break;
        case DialogKind::Tape: loadTape(path); break;
        case DialogKind::Disk: loadDisk(path); break;
        case DialogKind::Rom: loadRom(path); break;
        case DialogKind::Cartridge: loadCartridge(path); break;
        case DialogKind::DivMmcRom: loadDivMmcRom(path); break;
        case DialogKind::DivMmcCard: loadDivMmcCard(path); break;
        case DialogKind::Printout: savePrintout(path); break;
        case DialogKind::StateSave: saveStateFile(path); break;
        case DialogKind::StateLoad: loadStateFile(path); break;
        case DialogKind::RzxSave: saveRzxFile(path); break;
        case DialogKind::RzxLoad: loadRzxFile(path); break;
      }
      dialogSave_ = false;
    }

    u8 kempston = 0;
    input_.pollGamepads(kempston);
    machine_->setKempston(kempston);

    if (!paused_ && rewindEnabled_) {
      if (rewindStride_ == 0) {
        std::vector<u8> state;
        machine_->saveState(state);
        rewind_.push(std::move(state));
      }
      rewindStride_ = (rewindStride_ + 1) % kRewindStride;
    }

    if (!paused_) {
      for (int i = 0; i < speedMultiplier_; ++i) {
        // A breakpoint stops the frame early, so the machine is left with the
        // registers the user wants to look at.
        if (machine_->runFrame().has_value()) {
          // Half a frame of input is not a frame: drop it rather than write a
          // recording that cannot be replayed.
          rzx_.frameAborted();
          paused_ = true;
          break;
        }
        const bool wasPlaying = rzx_.playing();
        if (!rzx_.frameFinished(*machine_) && wasPlaying) {
          // A replay that ran out of frames has already switched itself off.
          status_ = "RZX replay finished";
          paused_ = true;
          break;
        }
      }
    }

    pushAudio();

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
    feedMouse();

    const ImGuiIO& io = ImGui::GetIO();
    framebufferScale_ = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    computeToolbarMetrics();

    // In fullscreen the toolbar floats over the picture and slides away again,
    // reappearing while the pointer rests against the top edge.
    const bool revealed = io.MousePos.y <= ui::kRevealZone || SDL_GetTicksNS() < toolbarRevealUntilNs_;
    toolbarVisible_ = fullscreen_ ? revealed : toolbarPinned_;

    if (toolbarVisible_) drawToolbar();
    updateLayout();
    if (showDebugger_) {
      const DebuggerRequest request = drawDebugger(*machine_, debuggerUi_, &showDebugger_);
      showDebugger_ = request.visible;
      if (request.resume) paused_ = false;
    }
    drawStatusOverlay();

    ImGui::Render();
    drawScreen();

    next += baseFrameNs / u64(speedMultiplier_);
    const u64 nowNs = SDL_GetTicksNS();
    if (nowNs < next) {
      SDL_DelayNS(next - nowNs);
    } else if (nowNs - next > baseFrameNs * 4) {
      next = nowNs;
    }
  }
}

void App::handleEvent(const SDL_Event& event) {
  if (imguiReady_) ImGui_ImplSDL3_ProcessEvent(&event);

  switch (event.type) {
    case SDL_EVENT_QUIT:
      running_ = false;
      break;

    case SDL_EVENT_KEY_DOWN:
      if (event.key.repeat) break;
      switch (event.key.key) {
        case SDLK_F1:
          showDebugger_ = !showDebugger_;
          break;
        case SDLK_F2:
          paused_ = !paused_;
          break;
        case SDLK_F3:
          paused_ = true;
          machine_->stepInstruction();
          break;
        case SDLK_F4:
          paused_ = true;
          stepOverInstruction(*machine_);
          break;
        case SDLK_F5:
          reset();
          break;
        case SDLK_F6:
          machine_->tape().setPlaying(!machine_->tape().playing());
          break;
        case SDLK_F7:
          machine_->tape().rewind();
          break;
        case SDLK_F8:
          // The plain key is the one that has to be quick, so the dialog lives
          // behind a modifier instead of the other way round.
          if ((event.key.mod & SDL_KMOD_SHIFT) != 0) {
            openSaveDialog(DialogKind::StateSave);
          } else {
            quickSaveState();
          }
          break;
        case SDLK_F9:
          if ((event.key.mod & SDL_KMOD_SHIFT) != 0) {
            openFileDialog(DialogKind::StateLoad);
          } else {
            quickLoadState();
          }
          break;
        case SDLK_F10:
          rewindOneFrame();
          break;
        case SDLK_F11:
          toggleFullscreen();
          break;
        case SDLK_ESCAPE:
          if (fullscreen_) toggleFullscreen();
          break;
        case SDLK_TAB:
          toolbarPinned_ = !toolbarPinned_;
          break;
        default:
          input_.handleKey(event.key.key, true, machine_->keyboard());
          break;
      }
      break;

    case SDL_EVENT_KEY_UP:
      input_.handleKey(event.key.key, false, machine_->keyboard());
      break;

    case SDL_EVENT_DROP_FILE: {
      const std::string path = event.drop.data != nullptr ? event.drop.data : "";
      if (path.empty()) break;
      const std::string extension = lowerExtension(path);
      if (extension == ".rom") {
        loadRom(path);
      } else if (extension == ".tap" || extension == ".tzx") {
        loadTape(path);
      } else if (extension == ".dsk" || extension == ".edsk") {
        loadDisk(path);
      } else if (extension == ".img") {
        loadDivMmcCard(path);
      } else if (extension == ".rbn") {
        loadStateFile(path);
      } else if (extension == ".rzx") {
        loadRzxFile(path);
      } else {
        loadSnapshot(path);
      }
      break;
    }

    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
      input_.openGamepads();
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::reset() {
  machine_->reset();
  status_ = "Reset";
}

void App::setMachine(MachineType type) {
  clearRewind();
  // A session belongs to the machine it was started on, so it goes before the
  // bus it was recording is torn down.
  rzx_.unbind();
  machine_ = std::make_unique<Machine>(type);
  machine_->setSampleRate(audio_.sampleRate() > 0 ? audio_.sampleRate() : 44100);
  rzx_.bind(*machine_);

  // A 48K ROM is not a valid 128K ROM and vice versa, so pick the matching set
  // instead of reusing whatever happened to be loaded before.
  bool loaded = loadRomFromAnywhere(*machine_, type, &romPath_);
  status_ = std::string("Machine: ") + machineName(type) +
            (loaded ? "  ROM: " + romPath_ : "  (no ROM found)");

  // A new machine has a new bus, so the plugins have to be loaded into it and
  // the user's choices replayed.
  scanPeripherals();
  applyPeripheralConfig();
  // The card lives in the frontend, so it goes back in whichever bus exists now.
  insertSdCard();
}

bool App::loadRom(const std::string& path) {
  if (!machine_->loadRom(path)) {
    status_ = "Failed to load ROM: " + path;
    return false;
  }
  romPath_ = path;
  // States are fingerprinted against the ROM, so a new ROM makes them all stale.
  clearRewind();
  status_ = "ROM loaded: " + path;
  return true;
}

bool App::loadSnapshot(const std::string& path) {
  // A .nex carries NextReg state and 2 MB of RAM, so it only makes sense on the
  // machine it was built for. Switching first keeps the user from having to
  // pick the type before every load.
  if (isNexPath(path) && !machine_->isNext()) setMachine(MachineType::Next);

  std::string error;
  if (!machine_->loadSnapshot(path, error)) {
    status_ = "Snapshot failed: " + error;
    return false;
  }
  status_ = std::string(isNexPath(path) ? "NEX loaded: " : "Snapshot loaded: ") + path;
  return true;
}

bool App::loadTape(const std::string& path) {
  std::string error;
  if (!machine_->loadTape(path, error)) {
    status_ = "Tape failed: " + error;
    return false;
  }
  status_ = "Tape loaded: " + machine_->tape().title();
  return true;
}

bool App::loadDisk(const std::string& path) {
  if (!machine_->hasDiskController()) {
    // Only the +3 is wired to the µPD765, so switch to it instead of refusing
    // the image the user just dropped on the window.
    setMachine(MachineType::Plus3);
  }
  std::string error;
  if (!machine_->loadDisk(path, error)) {
    status_ = "Disk failed: " + error;
    return false;
  }
  const formats::DiskImage& disk = machine_->disk();
  status_ = "Disk inserted: " + path + " (" + std::to_string(disk.tracks) + " tracks, " +
            std::to_string(disk.sides) + " side" + (disk.sides > 1 ? "s)" : ")");
  return true;
}

void App::openFileDialog(DialogKind kind) {
  if (dialogOpen_) return;
  static const SDL_DialogFileFilter romFilters[] = {{"Spectrum ROM", "rom"}, {"All files", "*"}};
  static const SDL_DialogFileFilter snapshotFilters[] = {
      {"Snapshots and programs", "sna;z80;nex"}, {"All files", "*"}};
  static const SDL_DialogFileFilter tapeFilters[] = {
      {"Tape images", "tap;tzx"}, {"All files", "*"}};
  static const SDL_DialogFileFilter diskFilters[] = {
      {"Disk images", "dsk;edsk"}, {"All files", "*"}};
  static const SDL_DialogFileFilter cartridgeFilters[] = {
      {"Cartridge images", "rom;bin;cart"}, {"All files", "*"}};
  static const SDL_DialogFileFilter divMmcRomFilters[] = {
      {"DivMMC ROM", "rom"}, {"All files", "*"}};
  static const SDL_DialogFileFilter divMmcCardFilters[] = {
      {"SD card images", "mmc;img;iso;bin"}, {"All files", "*"}};
  static const SDL_DialogFileFilter stateFilters[] = {
      {"RainbowNext save state", "rbn"}, {"All files", "*"}};
  static const SDL_DialogFileFilter rzxFilters[] = {
      {"RZX replays", "rzx"}, {"All files", "*"}};

  dialogKind_ = kind;
  dialogOpen_ = true;
  dialogSave_ = false;
  const SDL_DialogFileFilter* filters = romFilters;
  if (kind == DialogKind::Snapshot) filters = snapshotFilters;
  if (kind == DialogKind::Tape) filters = tapeFilters;
  if (kind == DialogKind::Disk) filters = diskFilters;
  if (kind == DialogKind::Cartridge) filters = cartridgeFilters;
  if (kind == DialogKind::DivMmcRom) filters = divMmcRomFilters;
  if (kind == DialogKind::DivMmcCard) filters = divMmcCardFilters;
  if (kind == DialogKind::StateLoad) filters = stateFilters;
  if (kind == DialogKind::RzxLoad) filters = rzxFilters;
  SDL_ShowOpenFileDialog(onFileDialog, this, window_, filters, 2, nullptr, false);
}

void App::openSaveDialog(DialogKind kind) {
  if (dialogOpen_) return;
  static const SDL_DialogFileFilter printoutFilters[] = {
      {"PBM image", "pbm"}, {"All files", "*"}};
  static const SDL_DialogFileFilter stateFilters[] = {
      {"RainbowNext save state", "rbn"}, {"All files", "*"}};
  static const SDL_DialogFileFilter rzxFilters[] = {
      {"RZX replays", "rzx"}, {"All files", "*"}};

  dialogKind_ = kind;
  dialogOpen_ = true;
  dialogSave_ = true;
  const SDL_DialogFileFilter* filters = printoutFilters;
  if (kind == DialogKind::StateSave) filters = stateFilters;
  if (kind == DialogKind::RzxSave) filters = rzxFilters;
  SDL_ShowSaveFileDialog(onFileDialog, this, window_, filters, 2, nullptr);
}

// ---------------------------------------------------------------------------
// Peripherals
// ---------------------------------------------------------------------------

bool App::loadCartridge(const std::string& path) {
  auto* cart = static_cast<Cartridge*>(machine_->expansion().find("cartridge"));
  if (cart == nullptr) return false;
  std::string error;
  if (!cart->loadFile(path, error)) {
    status_ = "Cartridge failed: " + error;
    return false;
  }
  // Loading an image is the user asking for it to run, so plug it in too.
  machine_->expansion().setEnabled("cartridge", true);
  syncConfigFromBus();
  status_ = "Cartridge: " + path;
  return true;
}

bool App::loadDivMmcRom(const std::string& path) {
  auto* div = static_cast<DivMmc*>(machine_->expansion().find("divmmc"));
  if (div == nullptr) return false;
  std::string error;
  if (!div->loadRomFile(path, error)) {
    status_ = "DivMMC ROM failed: " + error;
    return false;
  }
  machine_->expansion().setEnabled("divmmc", true);
  syncConfigFromBus();
  status_ = "DivMMC ROM: " + path;
  return true;
}

bool App::loadDivMmcCard(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    status_ = "SD card failed: cannot open " + path;
    return false;
  }
  std::vector<u8> image{std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>()};
  if (!in.eof() && in.fail()) {
    status_ = "SD card failed: cannot read " + path;
    return false;
  }

  // A card that does not fit the sector size is refused here rather than in the
  // module, because the message is the only thing the user gets to see.
  if (image.empty() || image.size() % 512 != 0) {
    status_ = "SD card failed: " + path + " is not a whole number of 512-byte sectors";
    return false;
  }

  sdImage_ = std::move(image);
  sdPath_ = path;
  insertSdCard();
  status_ = "SD card: " + path + " (" + std::to_string(sdImage_.size() / 512) +
            " sectors)";
  return true;
}

void App::insertSdCard() {
  if (sdImage_.empty()) return;
  auto* div = static_cast<DivMmc*>(machine_->expansion().find("divmmc"));
  if (div == nullptr) return;
  std::string error;
  if (!div->loadCard(sdImage_, error)) {
    status_ = "SD card failed: " + error;
    return;
  }
  // The card is useless unless the ports that reach it are decoded, and on the
  // Next the interface is built into the board, so enabling it is never wrong.
  machine_->expansion().setEnabled("divmmc", true);
  syncConfigFromBus();
}

void App::ejectSdCard() {
  sdImage_.clear();
  sdPath_.clear();
  auto* div = static_cast<DivMmc*>(machine_->expansion().find("divmmc"));
  if (div != nullptr) div->card().eject();
}

bool App::savePrintout(const std::string& path) {
  auto* printer = static_cast<ZxPrinter*>(machine_->expansion().find("printer"));
  if (printer == nullptr) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    status_ = "Cannot write " + path;
    return false;
  }
  out << printer->toPbm();
  status_ = "Printout saved: " + path;
  return true;
}

// The state is written whole before the file is touched, so a failed save
// leaves the previous one intact instead of half-overwritten.
bool App::saveStateFile(const std::string& path) {
  std::vector<u8> state;
  machine_->saveState(state);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    status_ = "Cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(state.data()), std::streamsize(state.size()));
  if (!out) {
    status_ = "Cannot write " + path;
    return false;
  }
  status_ = "State saved: " + path;
  return true;
}

bool App::loadStateFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    status_ = "State failed: cannot open " + path;
    return false;
  }
  std::vector<u8> state{std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>()};
  if (in.bad()) {
    status_ = "State failed: cannot read " + path;
    return false;
  }

  // A state names the machine it was written for, so switch to that machine
  // rather than refusing a file the user just picked.
  MachineType type = MachineType::Spectrum48;
  if (Machine::peekStateType(state, type) && type != machine_->type()) setMachine(type);

  std::string error;
  if (!machine_->loadState(state, error)) {
    status_ = "State failed: " + error;
    return false;
  }
  // The history belongs to the machine that was just replaced.
  clearRewind();
  status_ = "State loaded: " + path;
  return true;
}

// ---------------------------------------------------------------------------
// RZX replay
// ---------------------------------------------------------------------------

void App::startRzxRecording() {
  if (rzx_.playing()) {
    status_ = "Stop the replay before recording";
    return;
  }
  // The snapshot is taken now, so the recording starts at the point the user
  // pressed the menu item rather than wherever the machine happened to be
  // halfway through a frame.
  std::string error;
  if (!rzx_.startRecording(*machine_, error)) {
    status_ = "Cannot record: " + error;
    return;
  }
  paused_ = false;
  status_ = "Recording RZX - File > Stop recording to save it";
}

bool App::saveRzxFile(const std::string& path) {
  rzx::File file;
  if (!rzx_.takeRecording(file)) {
    status_ = "No recording to save";
    return false;
  }
  if (file.blocks.empty() || file.blocks.back().input.frames.empty()) {
    status_ = "Nothing was recorded";
    return false;
  }

  std::vector<u8> bytes;
  rzx::write(file, bytes);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    status_ = "Cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!out) {
    status_ = "Cannot write " + path;
    return false;
  }
  status_ = "RZX saved: " + path + " (" +
            std::to_string(file.blocks.back().input.frames.size()) + " frames)";
  return true;
}

bool App::loadRzxFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    status_ = "RZX failed: cannot open " + path;
    return false;
  }
  std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) {
    status_ = "RZX failed: cannot read " + path;
    return false;
  }

  rzx::File file;
  std::string error;
  if (!rzx::parse(bytes, file, error)) {
    status_ = "RZX failed: " + error;
    return false;
  }

  // The replay has to start from the snapshot the recording began with,
  // otherwise the input stream describes a machine that no longer exists.
  const rzx::SnapshotBlock* snapshot = nullptr;
  for (const rzx::Block& block : file.blocks) {
    if (block.kind == rzx::Block::Kind::Snapshot) {
      snapshot = &block.snapshot;
      break;
    }
  }
  if (snapshot == nullptr) {
    status_ = "RZX failed: no snapshot in the file";
    return false;
  }
  if (snapshot->format != "RBNS") {
    // Only our own state format is understood, and inventing a mapping from
    // SNA/SZX is not something a replay can afford to get wrong.
    status_ = "RZX failed: unsupported snapshot format \"" + snapshot->format + "\"";
    return false;
  }

  MachineType type = MachineType::Spectrum48;
  if (Machine::peekStateType(snapshot->data, type) && type != machine_->type()) setMachine(type);
  if (!machine_->loadState(snapshot->data, error)) {
    status_ = "RZX failed: " + error;
    return false;
  }

  clearRewind();
  rzx_.startPlayback(std::move(file));
  paused_ = false;
  status_ = "RZX playing: " + path + " (" + std::to_string(rzx_.totalFrames()) + " frames)";
  return true;
}

void App::stopRzx() {
  rzx_.cancel();
  status_ = "Recording discarded";
}

void App::quickSaveState() {  if (quickStatePath_.empty()) {
    status_ = "No writable folder for a quick save";
    return;
  }
  saveStateFile(quickStatePath_);
}

void App::quickLoadState() {
  if (quickStatePath_.empty()) {
    status_ = "No folder to look for a quick save in";
    return;
  }
  std::ifstream probe(quickStatePath_, std::ios::binary);
  if (!probe) {
    status_ = "No quick save yet - press F8 first";
    return;
  }
  probe.close();
  loadStateFile(quickStatePath_);
}

void App::rewindOneFrame() {
  std::vector<u8> state;
  if (!rewind_.pop(state)) {
    status_ = "Nothing to rewind yet";
    return;
  }
  std::string error;
  if (!machine_->loadState(state, error)) {
    // Every state in the buffer was written by the same machine, so one that
    // does not fit means the buffer is stale: drop it rather than keep failing.
    clearRewind();
    status_ = "Rewind failed: " + error;
    return;
  }
  paused_ = true;
  status_ = "Rewound to frame " + std::to_string(machine_->frames());
}

void App::clearRewind() {
  rewind_.clear();
  rewindStride_ = 0;
}

void App::loadConfig() {
  if (const char* dir = SDL_GetPrefPath("RainbowNext", "RainbowNext")) {
    configPath_ = std::string(dir) + "rainbow.cfg";
    quickStatePath_ = std::string(dir) + "quick.rbn";
  }
  config_.load(configPath_);

  // Plugins live next to the executable, which is where a downloaded build and
  // a development build both keep them.
  pluginDir_ = config_.pluginDir;
  if (pluginDir_.empty()) {
    if (const char* base = SDL_GetBasePath()) pluginDir_ = std::string(base) + "peripherals";
  }
  config_.pluginDir = pluginDir_;
  crt_.enabled = config_.crtEnabled;
  crt_.scanlines = config_.crtScanlines;
  crt_.mask = config_.crtMask;
}

void App::saveConfig() const {
  if (configPath_.empty()) return;
  Config copy = config_;
  copy.enabledPeripherals.clear();
  for (const auto& slot : machine_->expansion().slots()) {
    if (slot.enabled) copy.enabledPeripherals.push_back(slot.module->info().id);
  }
  copy.crtEnabled = crt_.enabled;
  copy.crtScanlines = crt_.scanlines;
  copy.crtMask = crt_.mask;
  copy.save(configPath_);
}

void App::syncConfigFromBus() {
  config_.enabledPeripherals.clear();
  for (const auto& slot : machine_->expansion().slots()) {
    if (slot.enabled) config_.enabledPeripherals.push_back(slot.module->info().id);
  }
  saveConfig();
}

void App::applyPeripheralConfig() {
  if (config_.enabledPeripherals.empty()) return;
  std::string list;
  for (const std::string& id : config_.enabledPeripherals) {
    if (!list.empty()) list += ',';
    list += id;
  }
  machine_->expansion().applyEnabledList(list);
}

void App::scanPeripherals() {
  ExpansionBus& bus = machine_->expansion();
  // Whatever is plugged in now stays plugged in: a rescan must not unplug a
  // module the user switched on.
  const std::string before = bus.enabledList();
  for (const std::string& id : pluginIds_) bus.remove(id);
  pluginIds_.clear();

  PluginLoader::Result result = PluginLoader::scan(pluginDir_, bus.host());
  pluginErrors_ = result.errors;
  for (auto& module : result.modules) {
    pluginIds_.push_back(module->info().id);
    bus.add(std::move(module), false);
  }
  bus.applyEnabledList(before);
}

void App::drawPeripheralsMenu() {
  ExpansionBus& bus = machine_->expansion();
  for (const auto& slot : bus.slots()) {
    const PeripheralInfo& info = slot.module->info();
    bool on = slot.enabled;
    if (ImGui::MenuItem(info.name.c_str(), nullptr, &on)) {
      bus.setEnabled(info.id, on);
      syncConfigFromBus();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\n\n%s", info.description.c_str(),
                        info.author.empty() ? info.id.c_str() : info.author.c_str());
    }
  }

  auto* cartridge = static_cast<Cartridge*>(bus.find("cartridge"));
  auto* printer = static_cast<ZxPrinter*>(bus.find("printer"));
  if (cartridge != nullptr || printer != nullptr) ImGui::Separator();
  if (cartridge != nullptr) {
    if (ImGui::MenuItem("Load cartridge image...")) openFileDialog(DialogKind::Cartridge);
    if (ImGui::MenuItem("Eject cartridge", nullptr, false, cartridge->loaded())) {
      cartridge->eject();
      bus.setEnabled("cartridge", false);
      syncConfigFromBus();
      status_ = "Cartridge ejected";
    }
  }
  auto* divmmc = static_cast<DivMmc*>(bus.find("divmmc"));
  if (divmmc != nullptr) {
    ImGui::Separator();
    if (ImGui::MenuItem("Load DivMMC ROM...")) openFileDialog(DialogKind::DivMmcRom);
    if (ImGui::MenuItem("Insert SD card image...")) {
      openFileDialog(DialogKind::DivMmcCard);
    }
    if (ImGui::MenuItem("Eject SD card", nullptr, false, divmmc->card().inserted())) {
      ejectSdCard();
      status_ = "SD card ejected";
    }
    ImGui::TextDisabled("ROM: %s", divmmc->romLoaded() ? "loaded" : "none");
    if (divmmc->card().inserted()) {
      ImGui::TextDisabled("Card: %s (%u sectors)", sdPath_.c_str(), divmmc->card().sectors());
    } else {
      ImGui::TextDisabled("Card: none");
    }
  }
  if (printer != nullptr) {
    if (ImGui::MenuItem("Save printout...", nullptr, false, printer->used())) {
      openSaveDialog(DialogKind::Printout);
    }
    if (ImGui::MenuItem("Clear printout", nullptr, false, printer->used())) printer->clear();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Rescan plugin folder")) {
    scanPeripherals();
    status_ = pluginErrors_.empty()
                  ? "Plugins: " + std::to_string(pluginIds_.size()) + " loaded"
                  : "Plugins: " + std::to_string(pluginErrors_.size()) + " failed to load";
  }
  if (!pluginErrors_.empty()) {
    ImGui::TextDisabled("%zu plugin(s) failed", pluginErrors_.size());
    if (ImGui::IsItemHovered()) {
      std::string all;
      for (const std::string& error : pluginErrors_) all += error + "\n";
      ImGui::SetTooltip("%s", all.c_str());
    }
  }
  ImGui::TextDisabled("Folder: %s", pluginDir_.c_str());
  if (ImGui::MenuItem("Open plugin folder")) SDL_OpenURL(("file://" + pluginDir_).c_str());
}

// The Kempston mouse follows the pointer while it is over the picture, which is
// the only place a mouse makes sense in a machine that has no window manager.
void App::feedMouse() {
  auto* mouse = static_cast<KempstonMouse*>(machine_->expansion().find("kempston-mouse"));
  if (mouse == nullptr || !machine_->expansion().enabled("kempston-mouse")) return;

  const ImGuiIO& io = ImGui::GetIO();
  const bool inside = io.MousePos.x >= screenMin_.x && io.MousePos.x < screenMax_.x &&
                      io.MousePos.y >= screenMin_.y && io.MousePos.y < screenMax_.y;
  if (inside) {
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      mouse->move(int(io.MouseDelta.x), int(io.MouseDelta.y));
    }
    // Bit 0 is the right button, 1 the left, 2 the middle.
    u8 buttons = 0;
    if (io.MouseDown[1]) buttons |= 0x01;
    if (io.MouseDown[0]) buttons |= 0x02;
    if (io.MouseDown[2]) buttons |= 0x04;
    mouse->setButtons(buttons);
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void App::pushAudio() {
  audio_.push(machine_->audioBuffer());
  machine_->clearAudio();
}

void App::computeToolbarMetrics() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::kToolbarPadX, ui::kToolbarPadY));
  toolbarRowHeight_ = ImGui::GetFrameHeight();
  ImGui::PopStyleVar();
  toolbarHeight_ = toolbarRowHeight_ + ui::kStripeHeight;
}

void App::drawToolbar() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, toolbarHeight_));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::kToolbarPadX, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::kToolbarPadX, ui::kToolbarPadY));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::toVec4(ui::kChrome));

  ImGui::Begin("##toolbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SetCursorPosY(0.0f);

  // Menu titles are buttons sized to the full row so the whole bar is clickable
  // and the label sits optically centred; ImGui's own BeginMenu would only give
  // us a text-height hit box pinned to the top of the row.
  float menuX = 6.0f;
  const auto menu = [&](const char* label, auto&& body) {
    ImGui::SetCursorPos(ImVec2(menuX, 0.0f));
    const ImVec2 buttonSize(ImGui::CalcTextSize(label).x + 2.0f * ui::kChipPadX,
                            toolbarRowHeight_);
    const bool open = ImGui::IsPopupOpen(label, ImGuiPopupFlags_None);

    ImGui::PushStyleColor(ImGuiCol_Button, ui::toVec4(open ? ui::kSurfaceActive : ui::kChrome));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::toVec4(ui::kSurfaceHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::toVec4(ui::kSurfaceActive));
    const bool pressed = ImGui::Button(label, buttonSize);
    ImGui::PopStyleColor(3);

    // A click on the title while its menu is open already closes the popup, so
    // only reopen when it was closed going into this frame.
    if (pressed && !open) ImGui::OpenPopup(label);

    if (open || pressed) {
      const ImVec2 itemMin = ImGui::GetItemRectMin();
      ImGui::SetNextWindowPos(ImVec2(itemMin.x, itemMin.y + buttonSize.y), ImGuiCond_Always);
    }
    if (ImGui::BeginPopup(label)) {
      body();
      ImGui::EndPopup();
    }
    menuX += buttonSize.x + ui::kChipGap * 0.5f;
  };

  menu("File", [this] {
    if (ImGui::MenuItem("Open ROM...")) openFileDialog(DialogKind::Rom);
    if (ImGui::MenuItem("Open Snapshot...")) openFileDialog(DialogKind::Snapshot);
    if (ImGui::MenuItem("Open Tape...")) openFileDialog(DialogKind::Tape);
    if (ImGui::MenuItem("Insert Disk...")) openFileDialog(DialogKind::Disk);
    ImGui::Separator();
    if (ImGui::MenuItem("Save state...", "Shift+F8")) openSaveDialog(DialogKind::StateSave);
    if (ImGui::MenuItem("Load state...", "Shift+F9")) openFileDialog(DialogKind::StateLoad);
    if (ImGui::MenuItem("Quick save", "F8")) quickSaveState();
    if (ImGui::MenuItem("Quick load", "F9")) quickLoadState();
    ImGui::Separator();
    if (rzx_.recording()) {
      if (ImGui::MenuItem("Stop recording...")) openSaveDialog(DialogKind::RzxSave);
      if (ImGui::MenuItem("Discard recording")) stopRzx();
    } else {
      if (ImGui::MenuItem("Record RZX", nullptr, false, !rzx_.playing())) startRzxRecording();
    }
    if (ImGui::MenuItem("Play RZX...")) openFileDialog(DialogKind::RzxLoad);
    if (rzx_.playing() && ImGui::MenuItem("Stop replay")) {
      rzx_.cancel();
      status_ = "Replay stopped";
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset", "F5")) reset();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) running_ = false;
  });

  menu("Tape", [this] {
    if (ImGui::MenuItem("Open Tape...")) openFileDialog(DialogKind::Tape);
    ImGui::Separator();
    Tape& tape = machine_->tape();
    const bool loaded = tape.loaded();
    if (ImGui::MenuItem(tape.playing() ? "Stop" : "Play", "F6", false, loaded)) {
      tape.setPlaying(!tape.playing());
    }
    if (ImGui::MenuItem("Rewind", "F7", false, loaded)) tape.rewind();
    ImGui::Separator();
    bool fastLoad = tape.fastLoad();
    if (ImGui::MenuItem("Instant load", nullptr, &fastLoad)) tape.setFastLoad(fastLoad);
    if (ImGui::MenuItem("Eject", nullptr, false, loaded)) tape.eject();
  });

  menu("Disk", [this] {
    if (ImGui::MenuItem("Insert Disk...")) openFileDialog(DialogKind::Disk);
    ImGui::Separator();
    const bool inserted = !machine_->disk().empty();
    if (ImGui::MenuItem("Eject", nullptr, false, inserted)) {
      machine_->ejectDisk();
      status_ = "Disk ejected";
    }
    const bool motor = machine_->fdc().motor();
    if (ImGui::MenuItem("Motor", nullptr, motor, inserted)) {
      machine_->fdc().setMotor(!motor);
    }
    ImGui::Separator();
    if (!machine_->hasDiskController()) {
      ImGui::TextDisabled("Select the +2A or +3 machine");
    } else if (!inserted) {
      ImGui::TextDisabled("No disk inserted");
    } else {
      ImGui::TextDisabled("Cylinder %d  head %d", machine_->fdc().cylinder(),
                          machine_->fdc().head());
    }
  });

  menu("Machine", [this] {
    for (MachineType type : AllMachines) {
      if (ImGui::MenuItem(machineName(type), nullptr, machine_->type() == type)) setMachine(type);
    }
  });

  menu("Peripherals", [this] { drawPeripheralsMenu(); });

  menu("View", [this] {
    ImGui::MenuItem("Debugger", "F1", &showDebugger_);
    ImGui::MenuItem("Pause", "F2", &paused_);
    if (ImGui::MenuItem("Step", "F3")) {
      paused_ = true;
      machine_->stepInstruction();
    }
    if (ImGui::MenuItem("Step over", "F4")) {
      paused_ = true;
      stepOverInstruction(*machine_);
    }
    if (ImGui::MenuItem("Rewind", "F10", false, rewind_.canRewind())) rewindOneFrame();
    if (ImGui::MenuItem("Rewind buffer", nullptr, &rewindEnabled_) && !rewindEnabled_) {
      clearRewind();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("CRT filter", nullptr, &crt_.enabled)) saveConfig();
    if (ImGui::BeginMenu("CRT strength", crt_.enabled)) {
      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderFloat("Scanlines", &crt_.scanlines, 0.0f, 1.0f, "%.2f");
      if (ImGui::IsItemDeactivatedAfterEdit()) saveConfig();
      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderFloat("Mask", &crt_.mask, 0.0f, 1.0f, "%.2f");
      if (ImGui::IsItemDeactivatedAfterEdit()) saveConfig();
      ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::MenuItem("Toolbar", "Tab", &toolbarPinned_);
    if (ImGui::MenuItem("Fullscreen", "F11", fullscreen_)) toggleFullscreen();
    ImGui::Separator();
    if (ImGui::BeginMenu("Speed")) {
      for (int speed : {1, 2, 4, 8}) {
        char label[8];
        std::snprintf(label, sizeof label, "%dx", speed);
        if (ImGui::MenuItem(label, nullptr, speedMultiplier_ == speed)) speedMultiplier_ = speed;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Sound")) {
      const Ay38912::Stereo mode = machine_->sound().stereoMode();
      const bool ay = machine_->hasAy();
      if (ImGui::MenuItem("Mono", nullptr, mode == Ay38912::Stereo::Mono, ay)) {
        machine_->sound().setStereoMode(Ay38912::Stereo::Mono);
      }
      if (ImGui::MenuItem("Stereo ABC", nullptr, mode == Ay38912::Stereo::ABC, ay)) {
        machine_->sound().setStereoMode(Ay38912::Stereo::ABC);
      }
      if (ImGui::MenuItem("Stereo ACB", nullptr, mode == Ay38912::Stereo::ACB, ay)) {
        machine_->sound().setStereoMode(Ay38912::Stereo::ACB);
      }
      ImGui::EndMenu();
    }
  });

  menu("Help", [this] { ImGui::MenuItem("About", nullptr, &showAbout_); });

  // State chips live on the right edge; they are pure decoration so they are
  // drawn straight onto the draw list instead of going through the layout.
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetWindowPos();
  const float chipHeight = ImGui::GetTextLineHeight() + 10.0f;
  const float chipY = origin.y + (toolbarRowHeight_ - chipHeight) * 0.5f;
  float chipX = viewport->Size.x - ui::kToolbarPadX;

  const auto placeChip = [&](const char* text, ImU32 background, ImU32 foreground) {
    chipX -= chipWidth(text);
    drawChip(drawList, ImVec2(origin.x + chipX, chipY), text, background, foreground, chipHeight);
    chipX -= ui::kChipGap;
  };
  if (paused_) placeChip("PAUSED", ui::kWarn, ui::kOnAccent);
  if (rzx_.recording()) placeChip("REC", ui::kDanger, ui::kOnAccent);
  if (rzx_.playing()) placeChip("REPLAY", ui::kAccent, ui::kOnAccent);
  if (!machine_->romLoaded()) placeChip("NO ROM", ui::kAccent, ui::kOnAccent);
  placeChip(machineName(machine_->type()), ui::kSurface, ui::kTextMuted);

  ui::drawSpectrumStripe(drawList, ImVec2(origin.x, origin.y + toolbarRowHeight_),
                         ImVec2(origin.x + viewport->Size.x, origin.y + toolbarHeight_));

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

void App::toggleFullscreen() {
  const bool target = !fullscreen_;
  if (!SDL_SetWindowFullscreen(window_, target)) {
    status_ = std::string("Fullscreen failed: ") + SDL_GetError();
    return;
  }
  fullscreen_ = target;
  // Keep the toolbar on screen for a moment so the new mode stays discoverable.
  toolbarRevealUntilNs_ = SDL_GetTicksNS() + 2'500'000'000ull;
  status_ = target ? "Fullscreen - F11 or Esc to leave, pointer to the top edge for the menu"
                   : "Windowed";
}

void App::updateLayout() {
  int windowWidth = 0;
  int windowHeight = 0;
  SDL_GetWindowSize(window_, &windowWidth, &windowHeight);

  const float top = (toolbarVisible_ && !fullscreen_) ? toolbarHeight_ : 0.0f;
  const float availableWidth = std::max(float(windowWidth), 1.0f);
  const float availableHeight = std::max(float(windowHeight) - top, 1.0f);

  float scale = std::min(availableWidth / float(Machine::FrameWidth),
                         availableHeight / float(Machine::FrameHeight));
  // Whole-number zoom keeps every Spectrum pixel the same size and square.
  if (scale >= 1.0f) {
    scale = std::floor(scale);
  } else {
    scale = std::max(scale, 0.1f);
  }

  const float width = float(Machine::FrameWidth) * scale;
  const float height = float(Machine::FrameHeight) * scale;
  screenMin_ = ImVec2(std::floor((availableWidth - width) * 0.5f),
                      top + std::floor((availableHeight - height) * 0.5f));
  screenMax_ = ImVec2(screenMin_.x + width, screenMin_.y + height);
}

void App::drawScreen() {
  // The layout is worked out in logical points; the shader multiplies by the
  // framebuffer scale, so Retina and 1080p TVs both get a pixel-exact picture
  // from the same maths.
  SDL_GPUCommandBuffer* command = screen_.beginFrame(
      machine_->framebuffer(), Machine::FrameWidth * int(sizeof(u32)));
  if (command == nullptr) return; // window minimised

  ImDrawData* drawData = ImGui::GetDrawData();
  ImGui_ImplSDLGPU3_PrepareDrawData(drawData, command);
  screen_.endFrame(command, drawData, screenMin_.x, screenMin_.y, screenMax_.x, screenMax_.y,
                   framebufferScale_, crt_);
}

void App::drawStatusOverlay() {
  if (!machine_->romLoaded()) {
    ImGui::SetNextWindowPos(ImVec2(screenMin_.x + 20.0f, screenMin_.y + 20.0f), ImGuiCond_Always);
    ImGui::Begin("ROM required", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("No Spectrum ROM is loaded.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Copy a 16K 48K ROM to roms/48.rom or use File > Open ROM.");
    ImGui::TextUnformatted("ROM images are copyrighted and are not bundled with RainbowNext.");
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Keyboard: F1 debugger, F2 pause, F5 reset, F6 tape, F7 rewind, arrows/Ctrl = Kempston.");
    ImGui::End();
  }

  if (!status_.empty()) {
    ImGui::SetNextWindowPos(ImVec2(screenMin_.x, screenMax_.y), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushFont(fonts_.small);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    const Tape& tape = machine_->tape();
    ImGui::Text("%s  |  %.1f Hz", status_.c_str(),
                double(machine_->timings().cpuHz) / double(machine_->tstatesPerFrameValue()));
    if (tape.loaded()) {
      ImGui::Text("Tape %zu/%zu %s  %s", tape.blockIndex() + 1, tape.blockCount(),
                  tape.playing() ? "PLAY" : (tape.atEnd() ? "END" : "STOP"),
                  tape.currentBlockName().c_str());
    }
    const formats::DiskImage& disk = machine_->disk();
    if (!disk.empty()) {
      const char* name = disk.path.empty() ? "image" : disk.path.c_str();
      const size_t slash = disk.path.find_last_of("/\\");
      if (slash != std::string::npos) name = disk.path.c_str() + slash + 1;
      ImGui::Text("Disk %s  %d tracks  %s", name, disk.tracks,
                  machine_->fdc().motor() ? "MOTOR" : "idle");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar();
  }

  if (showAbout_) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("About RainbowNext", &showAbout_);
    ImGui::TextUnformatted("RainbowNext - a from-scratch ZX Spectrum / Next emulator");
    ImGui::Separator();
    ImGui::TextUnformatted("Core: cycle-stepped Z80 + ULA with contention and floating bus.");
    ImGui::TextUnformatted("Frontend: SDL3 + Dear ImGui.");
    ImGui::TextUnformatted("ROMs are supplied by the user; none are bundled.");
    ImGui::End();
  }
}

} // namespace rainbow::frontend
