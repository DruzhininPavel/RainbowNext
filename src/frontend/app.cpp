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

bool App::init(const std::string& romPath) {
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
    if (ImGui::MenuItem("Open ROM...")) openFileDialog(false);
    if (ImGui::MenuItem("Open Snapshot...")) openFileDialog(true);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset", "F5")) reset();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) running_ = false;
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
    ImGui::TextUnformatted("Keyboard: F1 debugger, F2 pause, F5 reset, arrows/Ctrl = Kempston.");
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
    ImGui::Text("%s  |  %.1f Hz", status_.c_str(),
                double(machine_->timings().cpuHz) / double(machine_->tstatesPerFrameValue()));
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
