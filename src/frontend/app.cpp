#include "frontend/app.h"

#include <cstdio>
#include <vector>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "frontend/debugger.h"

namespace mynext::frontend {
namespace {

constexpr MachineType AllMachines[] = {
    MachineType::Spectrum48, MachineType::Spectrum128, MachineType::Plus2,
    MachineType::Plus3,      MachineType::Pentagon,    MachineType::Next,
};

std::string lowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

} // namespace

App::~App() { shutdown(); }

void SDLCALL App::onFileDialog(void* userdata, const char* const* filelist, int) {
  auto* app = static_cast<App*>(userdata);
  if (filelist != nullptr && filelist[0] != nullptr) app->pendingDialogResult_ = filelist[0];
  app->dialogOpen_ = false;
}

bool App::init(const std::string& romPath) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }

  window_ = SDL_CreateWindow("myNext", Machine::FrameWidth * 3, Machine::FrameHeight * 3,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (renderer_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetRenderVSync(renderer_, 0);

  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                               Machine::FrameWidth, Machine::FrameHeight);
  if (texture_ == nullptr) {
    std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);

  machine_ = std::make_unique<Machine>(MachineType::Spectrum48);
  audio_.init(44100);
  machine_->setSampleRate(audio_.sampleRate() > 0 ? audio_.sampleRate() : 44100);

  std::vector<std::string> candidates;
  if (!romPath.empty()) candidates.push_back(romPath);
  candidates.emplace_back("roms/48.rom");
  candidates.emplace_back("roms/48.ROM");
  candidates.emplace_back("48.rom");
  for (const std::string& candidate : candidates) {
    if (machine_->loadRom(candidate)) {
      romPath_ = candidate;
      status_ = "ROM loaded: " + candidate;
      break;
    }
  }
  if (!machine_->romLoaded()) {
    status_ = "No ROM loaded - put 48.rom into roms/ or use File > Open ROM";
  }

  input_.openGamepads();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer3_Init(renderer_);
  imguiReady_ = true;
  return true;
}

void App::shutdown() {
  input_.closeGamepads();
  audio_.shutdown();

  if (imguiReady_) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    imguiReady_ = false;
  }
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
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
      if (dialogForSnapshot_) {
        loadSnapshot(path);
      } else {
        loadRom(path);
      }
    }

    u8 kempston = 0;
    input_.pollGamepads(kempston);
    machine_->setKempston(kempston);

    if (!paused_) {
      for (int i = 0; i < speedMultiplier_; ++i) machine_->runFrame();
    }

    pushAudio();
    updateTexture();

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();

    drawMenuBar();
    if (showDebugger_) showDebugger_ = drawDebugger(*machine_, &showDebugger_);
    drawStatusOverlay();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 12, 12, 16, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);

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
        case SDLK_F5:
          reset();
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
      if (lowerExtension(path) == ".rom") {
        loadRom(path);
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
  machine_ = std::make_unique<Machine>(type);
  machine_->setSampleRate(audio_.sampleRate() > 0 ? audio_.sampleRate() : 44100);
  if (!romPath_.empty()) machine_->loadRom(romPath_);
  status_ = std::string("Machine: ") + machineName(type);
}

bool App::loadRom(const std::string& path) {
  if (!machine_->loadRom(path)) {
    status_ = "Failed to load ROM: " + path;
    return false;
  }
  romPath_ = path;
  status_ = "ROM loaded: " + path;
  return true;
}

bool App::loadSnapshot(const std::string& path) {
  std::string error;
  if (!machine_->loadSnapshot(path, error)) {
    status_ = "Snapshot failed: " + error;
    return false;
  }
  status_ = "Snapshot loaded: " + path;
  return true;
}

void App::openFileDialog(bool snapshot) {
  if (dialogOpen_) return;
  static const SDL_DialogFileFilter romFilters[] = {{"Spectrum ROM", "rom"}, {"All files", "*"}};
  static const SDL_DialogFileFilter snapshotFilters[] = {
      {"Snapshots", "sna;z80"}, {"All files", "*"}};

  dialogForSnapshot_ = snapshot;
  dialogOpen_ = true;
  SDL_ShowOpenFileDialog(onFileDialog, this, window_,
                         snapshot ? snapshotFilters : romFilters, 2, nullptr, false);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void App::pushAudio() {
  audio_.push(machine_->audioBuffer());
  machine_->clearAudio();
}

void App::updateTexture() {
  SDL_UpdateTexture(texture_, nullptr, machine_->framebuffer(), Machine::FrameWidth * int(sizeof(u32)));
}

void App::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open ROM...")) openFileDialog(false);
    if (ImGui::MenuItem("Open Snapshot...")) openFileDialog(true);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset", "F5")) reset();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) running_ = false;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Machine")) {
    for (MachineType type : AllMachines) {
      if (ImGui::MenuItem(machineName(type), nullptr, machine_->type() == type)) setMachine(type);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Debugger", "F1", &showDebugger_);
    ImGui::MenuItem("Pause", "F2", &paused_);
    if (ImGui::BeginMenu("Speed")) {
      for (int speed : {1, 2, 4, 8}) {
        char label[8];
        std::snprintf(label, sizeof label, "%dx", speed);
        if (ImGui::MenuItem(label, nullptr, speedMultiplier_ == speed)) speedMultiplier_ = speed;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    ImGui::MenuItem("About", nullptr, &showAbout_);
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

void App::drawStatusOverlay() {
  if (!machine_->romLoaded()) {
    ImGui::SetNextWindowPos(ImVec2(24.0f, 60.0f), ImGuiCond_Always);
    ImGui::Begin("ROM required", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("No Spectrum ROM is loaded.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Copy a 16K 48K ROM to roms/48.rom or use File > Open ROM.");
    ImGui::TextUnformatted("ROM images are copyrighted and are not bundled with myNext.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Keyboard: F1 debugger, F2 pause, F5 reset, arrows/Ctrl = Kempston.");
    ImGui::End();
  }

  if (!status_.empty()) {
    ImGui::SetNextWindowPos(ImVec2(24.0f, float(Machine::FrameHeight * 3) - 48.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    ImGui::Text("%s  |  %s  |  %.1f Hz", status_.c_str(), machineName(machine_->type()),
                double(machine_->timings().cpuHz) / double(machine_->tstatesPerFrameValue()));
    ImGui::End();
  }

  if (showAbout_) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("About myNext", &showAbout_);
    ImGui::TextUnformatted("myNext - a from-scratch ZX Spectrum / Next emulator");
    ImGui::Separator();
    ImGui::TextUnformatted("Core: cycle-stepped Z80 + ULA with contention and floating bus.");
    ImGui::TextUnformatted("Frontend: SDL3 + Dear ImGui.");
    ImGui::TextUnformatted("ROMs are supplied by the user; none are bundled.");
    ImGui::End();
  }
}

} // namespace mynext::frontend
