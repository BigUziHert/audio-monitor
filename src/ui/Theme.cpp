#include "ui/Theme.h"

#include <windows.h>

namespace audiomon::ui {
namespace {

constexpr ThemePalette kDarkPalette{
    IM_COL32(9, 13, 16, 255),       // background
    IM_COL32(18, 24, 28, 255),      // panel
    IM_COL32(23, 29, 33, 255),      // card
    IM_COL32(41, 48, 54, 255),      // border
    IM_COL32(242, 244, 248, 255),   // text
    IM_COL32(173, 181, 190, 255),   // mutedText
    IM_COL32(139, 96, 255, 255),    // accent
    IM_COL32(185, 147, 255, 255),   // accentText
    IM_COL32(109, 71, 231, 255),    // accentButton
    IM_COL32(48, 213, 73, 255),     // green
    IM_COL32(255, 174, 51, 255),    // amber
    IM_COL32(255, 58, 96, 255),     // red
    IM_COL32(14, 193, 226, 255),    // cyan
    IM_COL32(230, 63, 148, 255),    // pink
    IM_COL32(46, 51, 58, 255),      // sliderTrack
    IM_COL32(54, 60, 68, 255),      // sliderTicks
    IM_COL32(0, 0, 0, 80),          // shadow
    IM_COL32(48, 54, 61, 255),      // meterInactive
    IM_COL32(10, 16, 19, 255),      // graphBackground
    IM_COL32(26, 34, 39, 255),      // graphGrid
    IM_COL32(42, 47, 54, 255),      // disabledControl
    IM_COL32(39, 45, 51, 255),      // controlFill
    IM_COL32(22, 27, 33, 255),      // selectedSurface
    IM_COL32(92, 100, 109, 255),    // radioOutline
    IM_COL32(20, 53, 31, 255),      // activeStatusBackground
    IM_COL32(49, 42, 29, 255),      // idleStatusBackground
    IM_COL32(39, 30, 67, 255),      // accentSurface
    IM_COL32(66, 45, 130, 255),     // accentSurfaceStrong
    IM_COL32(61, 40, 86, 255),      // dangerSurface
    IM_COL32(38, 44, 51, 255),      // insetControl
    IM_COL32(255, 255, 255, 18),    // hoverOverlay
    IM_COL32(255, 255, 255, 32),    // pressedOverlay
    IM_COL32(169, 134, 255, 255),   // hoverOutline
    IM_COL32(12, 218, 64, 255),     // meterGreen
    IM_COL32(243, 224, 25, 255),    // meterYellow
    IM_COL32(255, 76, 27, 255),     // meterRed
    IM_COL32(0, 217, 57, 255),      // graphBarBottom
    IM_COL32(255, 255, 255, 255),   // onAccent
    IM_COL32(255, 255, 255, 255),   // sliderThumb
    IM_COL32(173, 181, 190, 255),   // sliderThumbBorder
};

constexpr ThemePalette kLightPalette{
    IM_COL32(241, 244, 248, 255),   // background
    IM_COL32(250, 251, 253, 255),   // panel
    IM_COL32(255, 255, 255, 255),   // card
    IM_COL32(204, 211, 220, 255),   // border
    IM_COL32(27, 31, 38, 255),      // text
    IM_COL32(92, 101, 113, 255),    // mutedText
    IM_COL32(103, 67, 220, 255),    // accent
    IM_COL32(91, 54, 190, 255),     // accentText
    IM_COL32(103, 67, 220, 255),    // accentButton
    IM_COL32(22, 145, 53, 255),     // green
    IM_COL32(181, 105, 0, 255),     // amber
    IM_COL32(210, 36, 72, 255),     // red
    IM_COL32(0, 126, 160, 255),     // cyan
    IM_COL32(190, 43, 120, 255),    // pink
    IM_COL32(207, 213, 221, 255),   // sliderTrack
    IM_COL32(181, 189, 200, 255),   // sliderTicks
    IM_COL32(26, 35, 50, 42),       // shadow
    IM_COL32(202, 209, 218, 255),   // meterInactive
    IM_COL32(236, 240, 245, 255),   // graphBackground
    IM_COL32(209, 216, 225, 255),   // graphGrid
    IM_COL32(218, 223, 230, 255),   // disabledControl
    IM_COL32(232, 236, 242, 255),   // controlFill
    IM_COL32(245, 242, 253, 255),   // selectedSurface
    IM_COL32(126, 136, 149, 255),   // radioOutline
    IM_COL32(220, 245, 226, 255),   // activeStatusBackground
    IM_COL32(255, 238, 210, 255),   // idleStatusBackground
    IM_COL32(236, 230, 252, 255),   // accentSurface
    IM_COL32(216, 204, 248, 255),   // accentSurfaceStrong
    IM_COL32(253, 225, 232, 255),   // dangerSurface
    IM_COL32(225, 230, 237, 255),   // insetControl
    IM_COL32(20, 26, 36, 14),       // hoverOverlay
    IM_COL32(20, 26, 36, 27),       // pressedOverlay
    IM_COL32(103, 67, 220, 255),    // hoverOutline
    IM_COL32(0, 151, 43, 255),      // meterGreen
    IM_COL32(184, 146, 0, 255),     // meterYellow
    IM_COL32(220, 61, 25, 255),     // meterRed
    IM_COL32(0, 159, 49, 255),      // graphBarBottom
    IM_COL32(255, 255, 255, 255),   // onAccent
    IM_COL32(103, 67, 220, 255),    // sliderThumb: visible on light cards and tracks
    IM_COL32(66, 37, 151, 255),     // sliderThumbBorder
};

ColorTheme requestedTheme = ColorTheme::Dark;
ThemePalette activePalette = kDarkPalette;

ColorTheme resolveTheme(ColorTheme theme) noexcept {
    if (theme != ColorTheme::System) return theme;

    DWORD useLightTheme = 0;
    DWORD bytes = sizeof(useLightTheme);
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &useLightTheme, &bytes);
    return result == ERROR_SUCCESS && useLightTheme != 0 ? ColorTheme::Light
                                                         : ColorTheme::Dark;
}

ImVec4 color(ImU32 value) noexcept {
    return ImGui::ColorConvertU32ToFloat4(value);
}

void applyPaletteToCurrentContext(const ThemePalette& palette, ColorTheme resolved) {
    if (!ImGui::GetCurrentContext()) return;

    if (resolved == ColorTheme::Light)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18;
    style.ChildRounding = 16;
    style.PopupRounding = 14;
    style.FrameRounding = 7;
    style.GrabRounding = 7;
    style.ScrollbarRounding = 8;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 0;
    style.WindowPadding = ImVec2(22, 22);
    style.FramePadding = ImVec2(12, 9);
    style.ItemSpacing = ImVec2(12, 12);
    style.GrabMinSize = 18;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = color(palette.text);
    colors[ImGuiCol_TextDisabled] = color(palette.mutedText);
    colors[ImGuiCol_WindowBg] = color(palette.background);
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = color(palette.panel);
    colors[ImGuiCol_Border] = color(palette.border);
    colors[ImGuiCol_FrameBg] = color(palette.card);
    colors[ImGuiCol_FrameBgHovered] = color(palette.selectedSurface);
    colors[ImGuiCol_FrameBgActive] = color(palette.accentSurfaceStrong);
    colors[ImGuiCol_Button] = color(palette.controlFill);
    colors[ImGuiCol_ButtonHovered] = color(palette.accentSurface);
    colors[ImGuiCol_ButtonActive] = color(palette.accentSurfaceStrong);
    colors[ImGuiCol_Header] = color(palette.accentSurface);
    colors[ImGuiCol_HeaderHovered] = color(palette.accentSurfaceStrong);
    colors[ImGuiCol_HeaderActive] = color(palette.accentSurfaceStrong);
    colors[ImGuiCol_SliderGrab] = color(palette.accent);
    colors[ImGuiCol_SliderGrabActive] = color(palette.accentText);
    colors[ImGuiCol_CheckMark] = color(palette.accent);
    colors[ImGuiCol_Separator] = color(palette.border);
    colors[ImGuiCol_TitleBg] = color(palette.panel);
    colors[ImGuiCol_TitleBgActive] = color(palette.panel);
    colors[ImGuiCol_NavCursor] = color(palette.accent);
}

void applyRememberedTheme() {
    const ColorTheme resolved = resolveTheme(requestedTheme);
    activePalette = resolved == ColorTheme::Light ? kLightPalette : kDarkPalette;
    applyPaletteToCurrentContext(activePalette, resolved);
}

} // namespace

void applyTheme(ColorTheme theme) {
    requestedTheme = theme;
    applyRememberedTheme();
}

void applyTheme() {
    applyRememberedTheme();
}

const ThemePalette& themePalette() noexcept {
    return activePalette;
}

} // namespace audiomon::ui
