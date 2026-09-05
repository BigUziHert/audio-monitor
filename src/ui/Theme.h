#pragma once
//
// Dashboard palette and conventional dBFS meter zones.
//
#include "config/Config.h"

#include <imgui.h>

namespace audiomon::ui {

// Semantic colors used by the custom-drawn mixer. Keeping them here makes a
// theme switch affect both ImGui widgets and the canvas-based dashboard.
struct ThemePalette {
    ImU32 background;
    ImU32 panel;
    ImU32 card;
    ImU32 border;
    ImU32 text;
    ImU32 mutedText;
    ImU32 accent;
    ImU32 accentText;
    ImU32 accentButton;
    ImU32 green;
    ImU32 amber;
    ImU32 red;
    ImU32 cyan;
    ImU32 pink;
    ImU32 sliderTrack;
    ImU32 sliderTicks;
    ImU32 shadow;
    ImU32 meterInactive;
    ImU32 graphBackground;
    ImU32 graphGrid;
    ImU32 disabledControl;
    ImU32 controlFill;
    ImU32 selectedSurface;
    ImU32 radioOutline;
    ImU32 activeStatusBackground;
    ImU32 idleStatusBackground;
    ImU32 accentSurface;
    ImU32 accentSurfaceStrong;
    ImU32 dangerSurface;
    ImU32 insetControl;
    ImU32 hoverOverlay;
    ImU32 pressedOverlay;
    ImU32 hoverOutline;

    // Distinct meter hues preserve the conventional dBFS zones, while
    // graphBarBottom supplies the cool end of the spectrum gradient.
    ImU32 meterGreen;
    ImU32 meterYellow;
    ImU32 meterRed;
    ImU32 graphBarBottom;
    ImU32 onAccent;
};

// Meter zones, matching the convention OBS uses so the colours mean the same
// thing to someone who already reads that mixer.
inline constexpr float kMeterGreenDb  = -20.0f;   // nominal below this
inline constexpr float kMeterYellowDb = -9.0f;    // hot above this
inline constexpr float kMeterRedDb    = -2.0f;    // clipping risk above this

// Applies and remembers an explicit preference. System is resolved from the
// Windows per-user app theme setting each time the theme is applied.
void applyTheme(ColorTheme theme);

// Reapplies the remembered preference (Dark until one is explicitly set).
// Renderer creation uses this overload so a recovered ImGui context keeps the
// user's selected theme.
void applyTheme();

const ThemePalette& themePalette() noexcept;

} // namespace audiomon::ui
