#pragma once

#include <imgui.h>

namespace rainbow::frontend::ui {

// Sizes are expressed in logical points. Dear ImGui rasterises glyphs at the
// display's pixel density on its own (it reads io.DisplayFramebufferScale), so
// a single set of metrics stays crisp on a Retina panel and on a 1080p TV.
inline constexpr float kFontSize = 17.0f;
inline constexpr float kSmallFontSize = 13.0f;

inline constexpr float kToolbarPadX = 14.0f;
inline constexpr float kToolbarPadY = 8.0f;
inline constexpr float kStripeHeight = 2.0f;
inline constexpr float kRevealZone = 8.0f;
inline constexpr float kChipGap = 8.0f;
inline constexpr float kChipPadX = 10.0f;

// Chrome palette: near-black surfaces with the Spectrum bright cyan as accent.
inline constexpr ImU32 kLetterbox = IM_COL32(6, 6, 9, 255);
inline constexpr ImU32 kChrome = IM_COL32(20, 20, 26, 255);
inline constexpr ImU32 kSurface = IM_COL32(32, 32, 42, 255);
inline constexpr ImU32 kSurfaceHover = IM_COL32(45, 45, 59, 255);
inline constexpr ImU32 kSurfaceActive = IM_COL32(58, 58, 76, 255);
inline constexpr ImU32 kHairline = IM_COL32(46, 46, 60, 255);
inline constexpr ImU32 kText = IM_COL32(233, 233, 241, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(156, 156, 174, 255);
inline constexpr ImU32 kAccent = IM_COL32(0, 215, 215, 255);
inline constexpr ImU32 kOnAccent = IM_COL32(10, 10, 14, 255);
inline constexpr ImU32 kWarn = IM_COL32(215, 215, 0, 255);
inline constexpr ImU32 kDanger = IM_COL32(224, 72, 72, 255);

struct Fonts {
  ImFont* regular = nullptr;
  ImFont* small = nullptr;
};

// Replaces the default 13 px ImGui font with a legible UI font when one can be
// found on the host, and falls back to the built-in font otherwise.
Fonts loadFonts();

void applyTheme(ImGuiStyle& style);
ImVec4 toVec4(ImU32 color);

// The six bright Spectrum colours, drawn as a thin band.
void drawSpectrumStripe(ImDrawList* drawList, ImVec2 min, ImVec2 max);

} // namespace rainbow::frontend::ui
