#include "frontend/ui.h"

#include <cstdio>

namespace mynext::frontend::ui {
namespace {

bool fileExists(const char* path) {
  if (std::FILE* file = std::fopen(path, "rb")) {
    std::fclose(file);
    return true;
  }
  return false;
}

// Probed in order; the first hit becomes the UI font. Falling back to the
// built-in font keeps the emulator usable on a bare system.
constexpr const char* kFontCandidates[] = {
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

const char* findSystemFont() {
  for (const char* path : kFontCandidates) {
    if (fileExists(path)) return path;
  }
  return nullptr;
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, a);
}

} // namespace

ImVec4 toVec4(ImU32 color) {
  return rgb(int((color >> IM_COL32_R_SHIFT) & 0xFF), int((color >> IM_COL32_G_SHIFT) & 0xFF),
             int((color >> IM_COL32_B_SHIFT) & 0xFF), float((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}

Fonts loadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  Fonts fonts;
  if (const char* path = findSystemFont()) {
    fonts.regular = io.Fonts->AddFontFromFileTTF(path, kFontSize);
    fonts.small = io.Fonts->AddFontFromFileTTF(path, kSmallFontSize);
  }
  if (fonts.regular == nullptr) {
    ImFontConfig config;
    config.SizePixels = kFontSize;
    fonts.regular = io.Fonts->AddFontDefault(&config);
  }
  if (fonts.small == nullptr) {
    ImFontConfig config;
    config.SizePixels = kSmallFontSize;
    fonts.small = io.Fonts->AddFontDefault(&config);
  }
  io.FontDefault = fonts.regular;
  return fonts;
}

void applyTheme(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(14.0f, 12.0f);
  style.FramePadding = ImVec2(10.0f, 7.0f);
  style.CellPadding = ImVec2(8.0f, 4.0f);
  style.ItemSpacing = ImVec2(8.0f, 7.0f);
  style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
  style.IndentSpacing = 20.0f;
  style.ScrollbarSize = 14.0f;
  style.GrabMinSize = 12.0f;

  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.WindowRounding = 8.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.GrabRounding = 6.0f;
  style.TabRounding = 6.0f;

  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.SeparatorTextBorderSize = 1.0f;

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = toVec4(kText);
  colors[ImGuiCol_TextDisabled] = toVec4(kTextMuted);
  colors[ImGuiCol_WindowBg] = toVec4(kChrome);
  colors[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0.0f);
  colors[ImGuiCol_PopupBg] = rgb(26, 26, 34, 0.98f);
  colors[ImGuiCol_Border] = toVec4(kHairline);
  colors[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
  colors[ImGuiCol_FrameBg] = toVec4(kSurface);
  colors[ImGuiCol_FrameBgHovered] = toVec4(kSurfaceHover);
  colors[ImGuiCol_FrameBgActive] = toVec4(kSurfaceActive);
  colors[ImGuiCol_TitleBg] = toVec4(kChrome);
  colors[ImGuiCol_TitleBgActive] = rgb(28, 28, 38);
  colors[ImGuiCol_TitleBgCollapsed] = toVec4(kChrome);
  colors[ImGuiCol_MenuBarBg] = toVec4(kChrome);
  colors[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.25f);
  colors[ImGuiCol_ScrollbarGrab] = toVec4(kSurfaceActive);
  colors[ImGuiCol_ScrollbarGrabHovered] = rgb(78, 78, 98);
  colors[ImGuiCol_ScrollbarGrabActive] = toVec4(kAccent);
  colors[ImGuiCol_CheckMark] = toVec4(kAccent);
  colors[ImGuiCol_CheckboxSelectedBg] = toVec4(kAccent);
  colors[ImGuiCol_SliderGrab] = toVec4(kAccent);
  colors[ImGuiCol_SliderGrabActive] = rgb(120, 245, 245);
  colors[ImGuiCol_Button] = toVec4(kSurface);
  colors[ImGuiCol_ButtonHovered] = toVec4(kSurfaceHover);
  colors[ImGuiCol_ButtonActive] = toVec4(kSurfaceActive);
  colors[ImGuiCol_Header] = toVec4(kSurface);
  colors[ImGuiCol_HeaderHovered] = toVec4(kSurfaceHover);
  colors[ImGuiCol_HeaderActive] = toVec4(kSurfaceActive);
  colors[ImGuiCol_Separator] = toVec4(kHairline);
  colors[ImGuiCol_SeparatorHovered] = toVec4(kAccent);
  colors[ImGuiCol_SeparatorActive] = toVec4(kAccent);
  colors[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0.0f);
  colors[ImGuiCol_ResizeGripHovered] = toVec4(kAccent);
  colors[ImGuiCol_ResizeGripActive] = toVec4(kAccent);
  colors[ImGuiCol_InputTextCursor] = toVec4(kAccent);
  colors[ImGuiCol_Tab] = toVec4(kSurface);
  colors[ImGuiCol_TabHovered] = toVec4(kSurfaceHover);
  colors[ImGuiCol_TabSelected] = toVec4(kSurfaceActive);
  colors[ImGuiCol_TabSelectedOverline] = toVec4(kAccent);
  colors[ImGuiCol_TabDimmed] = toVec4(kChrome);
  colors[ImGuiCol_TabDimmedSelected] = toVec4(kSurface);
  colors[ImGuiCol_TabDimmedSelectedOverline] = toVec4(kHairline);
  colors[ImGuiCol_PlotLines] = toVec4(kAccent);
  colors[ImGuiCol_PlotLinesHovered] = toVec4(kWarn);
  colors[ImGuiCol_PlotHistogram] = toVec4(kAccent);
  colors[ImGuiCol_PlotHistogramHovered] = toVec4(kWarn);
  colors[ImGuiCol_TableHeaderBg] = toVec4(kSurface);
  colors[ImGuiCol_TableBorderStrong] = toVec4(kHairline);
  colors[ImGuiCol_TableBorderLight] = rgb(36, 36, 46);
  colors[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
  colors[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
  colors[ImGuiCol_TextLink] = toVec4(kAccent);
  colors[ImGuiCol_TextSelectedBg] = rgb(0, 215, 215, 0.35f);
  colors[ImGuiCol_TreeLines] = toVec4(kHairline);
  colors[ImGuiCol_DragDropTarget] = toVec4(kAccent);
  colors[ImGuiCol_DragDropTargetBg] = rgb(0, 215, 215, 0.15f);
  colors[ImGuiCol_UnsavedMarker] = toVec4(kWarn);
  colors[ImGuiCol_NavCursor] = toVec4(kAccent);
  colors[ImGuiCol_NavWindowingHighlight] = toVec4(kAccent);
  colors[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 0.55f);
  colors[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.60f);
}

void drawSpectrumStripe(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
  static constexpr ImU32 kBand[] = {
      IM_COL32(215, 0, 0, 255),   IM_COL32(215, 215, 0, 255), IM_COL32(0, 215, 0, 255),
      IM_COL32(0, 215, 215, 255), IM_COL32(0, 0, 215, 255),   IM_COL32(215, 0, 215, 255),
  };
  constexpr int kCount = int(sizeof(kBand) / sizeof(kBand[0]));
  const float width = (max.x - min.x) / float(kCount);
  for (int i = 0; i < kCount; ++i) {
    const float x0 = min.x + width * float(i);
    const float x1 = (i == kCount - 1) ? max.x : min.x + width * float(i + 1);
    drawList->AddRectFilled(ImVec2(x0, min.y), ImVec2(x1, max.y), kBand[i]);
  }
}

} // namespace mynext::frontend::ui
