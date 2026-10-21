#include "frontend/app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "frontend/debugger.h"
#include "frontend/ui.h"

namespace mynext::frontend {
namespace {

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

} // namespace

App::~App() { shutdown(); }

void SDLCALL App::onFileDialog(void* userdata, const char* const* filelist, int) {
  auto* app = static_cast<App*>(userdata);
  if (filelist != nullptr && filelist[0] != nullptr) app->pendingDialogResult_ = filelist[0];
  app->dialogOpen_ = false;
}

bool App::init(const Options& options) {
  SDL_SetAppMetadata("myNext", "0.1.0", "com.mynext.emulator");

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

  bool loaded = false;
  if (!options.rom.empty() && machine_->loadRomImage(options.rom)) {
    romPath_ = options.rom;
    loaded = true;
  }
  if (!loaded) loaded = loadRomFor(*machine_, machine_->type(), "roms/", &romPath_);
  if (!loaded) loaded = loadRomFor(*machine_, machine_->type(), "", &romPath_);
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

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ui::applyTheme(ImGui::GetStyle());
  fonts_ = ui::loadFonts();
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
      switch (dialogKind_) {
        case DialogKind::Snapshot: loadSnapshot(path); break;
        case DialogKind::Tape: loadTape(path); break;
        case DialogKind::Disk: loadDisk(path); break;
        case DialogKind::Rom: loadRom(path); break;
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

    const ImGuiIO& io = ImGui::GetIO();
    framebufferScale_ = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    computeToolbarMetrics();

    // In fullscreen the toolbar floats over the picture and slides away again,
    // reappearing while the pointer rests against the top edge.
    const bool revealed = io.MousePos.y <= ui::kRevealZone || SDL_GetTicksNS() < toolbarRevealUntilNs_;
    toolbarVisible_ = fullscreen_ ? revealed : toolbarPinned_;

    if (toolbarVisible_) drawToolbar();
    updateLayout();
    if (showDebugger_) showDebugger_ = drawDebugger(*machine_, &showDebugger_);
    drawStatusOverlay();

    ImGui::Render();
    drawScreen();
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
        case SDLK_F6:
          machine_->tape().setPlaying(!machine_->tape().playing());
          break;
        case SDLK_F7:
          machine_->tape().rewind();
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

  // A 48K ROM is not a valid 128K ROM and vice versa, so pick the matching set
  // instead of reusing whatever happened to be loaded before.
  bool loaded = loadRomFor(*machine_, type, "roms/", &romPath_);
  if (!loaded) loaded = loadRomFor(*machine_, type, "", &romPath_);
  status_ = std::string("Machine: ") + machineName(type) +
            (loaded ? "  ROM: " + romPath_ : "  (no ROM found)");
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
      {"Snapshots", "sna;z80"}, {"All files", "*"}};
  static const SDL_DialogFileFilter tapeFilters[] = {
      {"Tape images", "tap;tzx"}, {"All files", "*"}};
  static const SDL_DialogFileFilter diskFilters[] = {
      {"Disk images", "dsk;edsk"}, {"All files", "*"}};

  dialogKind_ = kind;
  dialogOpen_ = true;
  const SDL_DialogFileFilter* filters = romFilters;
  if (kind == DialogKind::Snapshot) filters = snapshotFilters;
  if (kind == DialogKind::Tape) filters = tapeFilters;
  if (kind == DialogKind::Disk) filters = diskFilters;
  SDL_ShowOpenFileDialog(onFileDialog, this, window_, filters, 2, nullptr, false);
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

  menu("View", [this] {
    ImGui::MenuItem("Debugger", "F1", &showDebugger_);
    ImGui::MenuItem("Pause", "F2", &paused_);
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
      const Ay38912::Stereo mode = machine_->ay().stereo();
      const bool ay = machine_->hasAy();
      if (ImGui::MenuItem("Mono", nullptr, mode == Ay38912::Stereo::Mono, ay)) {
        machine_->ay().setStereo(Ay38912::Stereo::Mono);
      }
      if (ImGui::MenuItem("Stereo ABC", nullptr, mode == Ay38912::Stereo::ABC, ay)) {
        machine_->ay().setStereo(Ay38912::Stereo::ABC);
      }
      if (ImGui::MenuItem("Stereo ACB", nullptr, mode == Ay38912::Stereo::ACB, ay)) {
        machine_->ay().setStereo(Ay38912::Stereo::ACB);
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
  // Everything we hand to SDL stays in logical points; the render scale turns
  // that into the physical backing store, so Retina and 1080p TVs both get a
  // pixel-exact picture from the same layout maths.
  SDL_SetRenderScale(renderer_, framebufferScale_, framebufferScale_);
  SDL_SetRenderDrawColor(renderer_, 6, 6, 9, 255);
  SDL_RenderClear(renderer_);

  const SDL_FRect destination = {screenMin_.x, screenMin_.y, screenMax_.x - screenMin_.x,
                                 screenMax_.y - screenMin_.y};
  SDL_RenderTexture(renderer_, texture_, nullptr, &destination);
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
    ImGui::TextUnformatted("ROM images are copyrighted and are not bundled with myNext.");
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
