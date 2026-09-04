#include "ui/MixerWindow.h"
#include "util/Startup.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace audiomon::ui {
namespace {
constexpr ImU32 background = IM_COL32(9, 13, 16, 255), panel = IM_COL32(18, 24, 28, 255),
                card = IM_COL32(23, 29, 33, 255);
constexpr ImU32 border = IM_COL32(41, 48, 54, 255), white = IM_COL32(242, 244, 248, 255),
                gray = IM_COL32(173, 181, 190, 255);
constexpr ImU32 purple = IM_COL32(139, 96, 255, 255), green = IM_COL32(48, 213, 73, 255),
                amber = IM_COL32(255, 174, 51, 255);
constexpr ImU32 red = IM_COL32(255, 58, 96, 255), cyan = IM_COL32(14, 193, 226, 255),
                pink = IM_COL32(230, 63, 148, 255);
enum Icon {
    Wave,
    Headphones,
    Chat,
    Mic,
    Speaker,
    Muted,
    Arrow,
    Down,
    Check,
    Refresh,
    Plus,
    Screen,
    Clock,
    Link,
    Bars,
    Gear,
    Stop,
    Play,
    Minus,
    Maximize,
    Close,
    Sliders,
    Info,
    Puzzle,
    Dot
};
struct IconChoice {
    const char *key;
    Icon icon;
};
constexpr IconChoice iconChoices[] = {{"microphone", Mic}, {"speaker", Speaker}, {"chat", Chat},
                                      {"headphones", Headphones}, {"screen", Screen}, {"wave", Wave}};
struct Canvas {
    ImDrawList *dl;
    ImVec2 origin;
    float s;
    ImVec2 p(float x, float y) const {
        return {origin.x + x * s, origin.y + y * s};
    }
    void rect(float x, float y, float w, float h, ImU32 color, float rounding = 14,
              bool stroke = true) const {
        dl->AddRectFilled(p(x, y), p(x + w, y + h), color, rounding * s);
        if (stroke)
            dl->AddRect(p(x, y), p(x + w, y + h), border, rounding * s, 0, s);
    }
    void line(float x, float y, float xx, float yy, ImU32 color, float thickness = 2) const {
        dl->AddLine(p(x, y), p(xx, yy), color, thickness * s);
    }
    ImFont *fontFor(bool bold) const {
        auto &fonts = ImGui::GetIO().Fonts->Fonts;
        return bold && fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
    }
    ImVec2 textSize(const std::string &label, float size, bool bold) const {
        const auto measured = fontFor(bold)->CalcTextSizeA(size * s, FLT_MAX, 0, label.c_str());
        return {measured.x / s, measured.y / s};
    }
    void text(float x, float y, const std::string &label, float size = 21, ImU32 color = white,
              bool bold = false, float maxWidth = 0) const {
        ImVec4 clip(origin.x + x * s, origin.y + y * s, origin.x + (x + maxWidth) * s,
                    origin.y + (y + size + 8) * s);
        dl->AddText(fontFor(bold), size * s, p(x, y), color, label.c_str(), nullptr, 0,
                    maxWidth > 0 ? &clip : nullptr);
    }
    void scrollingText(float x, float y, const std::string &label, float size, ImU32 color,
                       bool bold, float maxWidth) const {
        const auto measured = textSize(label, size, bold);
        if (measured.x <= maxWidth) {
            text(x, y, label, size, color, bold, maxWidth);
            return;
        }

        // Pause at both ends so long names remain readable instead of moving constantly.
        constexpr float speed = 34.f, hold = 1.15f;
        const float travel = measured.x - maxWidth;
        const float travelTime = travel / speed;
        const float cycle = hold * 2 + travelTime * 2;
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), cycle));
        float offset = 0;
        if (phase > hold && phase <= hold + travelTime)
            offset = (phase - hold) * speed;
        else if (phase > hold + travelTime && phase <= hold * 2 + travelTime)
            offset = travel;
        else if (phase > hold * 2 + travelTime)
            offset = travel - (phase - hold * 2 - travelTime) * speed;

        const ImVec4 clip(origin.x + x * s, origin.y + y * s,
                          origin.x + (x + maxWidth) * s, origin.y + (y + size + 8) * s);
        dl->AddText(fontFor(bold), size * s, p(x - offset, y), color, label.c_str(), nullptr, 0, &clip);
    }
    void wrappedText(float x, float y, const std::string &label, float size, ImU32 color,
                     float maxWidth, float maxHeight) const {
        const ImVec4 clip(origin.x + x * s, origin.y + y * s,
                          origin.x + (x + maxWidth) * s, origin.y + (y + maxHeight) * s);
        dl->AddText(fontFor(false), size * s, p(x, y), color, label.c_str(), nullptr,
                    maxWidth * s, &clip);
    }
    void centeredText(float x, float y, const std::string &label, float size, ImU32 color) const {
        const auto measured = textSize(label, size, false);
        text(x - measured.x / 2, y - measured.y / 2, label, size, color);
    }
    void alignedIconLabel(Icon type, float iconX, float textX, float centerY,
                          const std::string &label, float size, float iconSize,
                          ImU32 iconColor, ImU32 textColor = white) const {
        icon(type, iconX, centerY, iconColor, iconSize);
        const auto measured = textSize(label, size, true);
        text(textX, centerY - measured.y / 2, label, size, textColor, true);
    }
    void centeredIconLabel(Icon type, float x, float y, const std::string &label, float size,
                           float iconSize, ImU32 iconColor, ImU32 textColor = white) const {
        const auto measured = textSize(label, size, true);
        const float gap = 18, left = x - (iconSize + gap + measured.x) / 2;
        icon(type, left + iconSize / 2, y, iconColor, iconSize);
        text(left + iconSize + gap, y - measured.y / 2, label, size, textColor, true);
    }
    bool hit(const char *id, float x, float y, float w, float h, const char *tooltip = nullptr) const {
        ImGui::SetCursorScreenPos(p(x, y));
        bool clicked = ImGui::InvisibleButton(id, {w * s, h * s}, ImGuiButtonFlags_EnableNav);
        if (ImGui::IsItemHovered() && tooltip)
            ImGui::SetTooltip("%s", tooltip);
        // Popup dismissal restores focus, but only keyboard/gamepad navigation
        // should leave a visible outline after the mouse action is finished.
        if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
            dl->AddRect(p(x, y), p(x + w, y + h), purple, 8 * s, 0, 2 * s);
        return clicked;
    }
    void icon(Icon kind, float x, float y, ImU32 color = white, float size = 28) const {
        Canvas c{dl, p(x - size / 2, y - size / 2), s * size / 32};
        auto l = [&](float a, float b, float d, float e, float t = 2.5f) { c.line(a, b, d, e, color, t); };
        auto circle = [&](float a, float b, float r) {
            dl->AddCircle(c.p(a, b), r * c.s, color, 24, 2.5f * c.s);
        };
        switch (kind) {
        case Wave:
            for (int i = 0; i < 5; ++i) {
                float h = i == 2 ? 27.f : (i % 2 ? 17.f : 5.f);
                l(3 + i * 6.f, 16 - h / 2, 3 + i * 6.f, 16 + h / 2, 3);
            }
            break;
        case Headphones:
            dl->PathArcTo(c.p(16, 16), 12 * c.s, 3.14159f, 6.28319f, 20);
            dl->PathStroke(color, 0, 3 * c.s);
            c.rect(3, 15, 6, 13, color, 3, false);
            c.rect(23, 15, 6, 13, color, 3, false);
            break;
        case Chat:
            circle(16, 14, 12);
            l(7, 23, 3, 30);
            l(3, 30, 13, 25);
            break;
        case Mic:
            c.rect(12, 2, 8, 19, color, 4, false);
            l(7, 14, 7, 19);
            l(25, 14, 25, 19);
            dl->PathArcTo(c.p(16, 18), 9 * c.s, 0, 3.14159f, 18);
            dl->PathStroke(color, 0, 2.5f * c.s);
            l(16, 27, 16, 31);
            l(10, 31, 22, 31);
            break;
        case Speaker:
        case Muted: {
            const ImVec2 speaker[] = {c.p(2, 12), c.p(7, 12), c.p(16, 5),
                                      c.p(16, 27), c.p(7, 20), c.p(2, 20)};
            dl->AddConcavePolyFilled(speaker, IM_ARRAYSIZE(speaker), color);
            if (kind == Muted) {
                l(21, 12, 30, 22);
                l(30, 12, 21, 22);
            } else {
                dl->PathArcTo(c.p(15, 16), 10 * c.s, -.8f, .8f, 12);
                dl->PathStroke(color, 0, 2 * c.s);
                dl->PathArcTo(c.p(15, 16), 16 * c.s, -.7f, .7f, 12);
                dl->PathStroke(color, 0, 2 * c.s);
            }
            break;
        }
        case Arrow:
            l(12, 7, 21, 16);
            l(21, 16, 12, 25);
            break;
        case Down:
            l(7, 12, 16, 21);
            l(16, 21, 25, 12);
            break;
        case Check:
            l(7, 16, 13, 22, 3);
            l(13, 22, 26, 9, 3);
            break;
        case Plus:
            l(16, 4, 16, 28);
            l(4, 16, 28, 16);
            break;
        case Refresh:
            dl->PathArcTo(c.p(16, 16), 12 * c.s, .4f, 5.5f, 24);
            dl->PathStroke(color, 0, 2.5f * c.s);
            l(25, 3, 25, 11);
            l(18, 11, 25, 11);
            break;
        case Screen:
            dl->AddRect(c.p(2, 3), c.p(30, 23), color, 2 * c.s, 0, 2.5f * c.s);
            l(16, 23, 16, 30);
            l(8, 30, 24, 30);
            break;
        case Clock:
            circle(16, 18, 12);
            l(16, 18, 16, 10);
            l(12, 1, 20, 1);
            l(16, 1, 16, 5);
            l(25, 5, 28, 8);
            break;
        case Link:
            circle(11, 19, 8);
            circle(23, 12, 8);
            l(12, 18, 21, 12, 3);
            break;
        case Bars:
            c.rect(4, 18, 5, 12, color, 1, false);
            c.rect(13, 4, 5, 26, color, 1, false);
            c.rect(22, 12, 5, 18, color, 1, false);
            break;
        case Gear:
            circle(16, 16, 10);
            circle(16, 16, 4);
            for (int i = 0; i < 8; ++i) {
                float a = i * 6.283185f / 8;
                l(16 + 10 * std::cos(a), 16 + 10 * std::sin(a), 16 + 15 * std::cos(a), 16 + 15 * std::sin(a),
                  4);
            }
            break;
        case Stop:
            dl->AddRect(c.p(3, 3), c.p(29, 29), color, 5 * c.s, 0, 2.5f * c.s);
            break;
        case Play:
            dl->AddTriangleFilled(c.p(6, 2), c.p(29, 16), c.p(6, 30), color);
            break;
        case Minus:
            l(6, 16, 26, 16, 2);
            break;
        case Maximize:
            dl->AddRect(c.p(7, 7), c.p(25, 25), color, 2 * c.s, 0, 2 * c.s);
            break;
        case Close:
            l(8, 8, 24, 24, 2);
            l(24, 8, 8, 24, 2);
            break;
        case Sliders:
            l(4, 9, 28, 9, 2);
            l(4, 16, 28, 16, 2);
            l(4, 23, 28, 23, 2);
            dl->AddCircleFilled(c.p(11, 9), 3.5f * c.s, color, 16);
            dl->AddCircleFilled(c.p(22, 16), 3.5f * c.s, color, 16);
            dl->AddCircleFilled(c.p(15, 23), 3.5f * c.s, color, 16);
            break;
        case Info:
            circle(16, 16, 12);
            dl->AddCircleFilled(c.p(16, 10), 1.9f * c.s, color, 12);
            l(16, 15, 16, 23, 2.2f);
            break;
        case Puzzle:
            // Four-lobed puzzle-piece outline used by the Plugins settings page.
            dl->PathLineTo(c.p(5, 6));
            dl->PathLineTo(c.p(12, 6));
            dl->PathBezierCubicCurveTo(c.p(10, 1), c.p(20, 1), c.p(18, 6));
            dl->PathLineTo(c.p(26, 6));
            dl->PathLineTo(c.p(26, 13));
            dl->PathBezierCubicCurveTo(c.p(31, 11), c.p(31, 21), c.p(26, 19));
            dl->PathLineTo(c.p(26, 27));
            dl->PathLineTo(c.p(18, 27));
            dl->PathBezierCubicCurveTo(c.p(20, 32), c.p(10, 32), c.p(12, 27));
            dl->PathLineTo(c.p(5, 27));
            dl->PathLineTo(c.p(5, 19));
            dl->PathBezierCubicCurveTo(c.p(0, 21), c.p(0, 11), c.p(5, 13));
            dl->PathStroke(color, ImDrawFlags_Closed, 2.3f * c.s);
            break;
        case Dot:
            dl->AddCircleFilled(c.p(16, 16), 16 * c.s, color, 24);
            break;
        }
    }
    void badge(Icon type, float x, float y, ImU32 color) const {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(color);
        v.w = .13f;
        dl->AddCircleFilled(p(x, y), 31 * s, ImGui::ColorConvertFloat4ToU32(v), 40);
        icon(type, x, y, color, 34);
    }
    void infoMark(float x, float y, ImU32 color, float size = 18) const {
        const float radius = size / 2;
        dl->AddCircle(p(x, y), radius * s, color, 28, 1.7f * s);
        const float fontSize = size * .72f;
        const auto measured = textSize("i", fontSize, true);
        text(x - measured.x / 2, y - measured.y / 2 - .5f, "i", fontSize, color, true);
    }
    bool volume(const char *id, float x, float y, float width, float &gain) const {
        // Keep the native slider's keyboard navigation; replace its appearance.
        ImGui::SetCursorScreenPos(p(x, y));
        ImGui::SetNextItemWidth(width * s);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
        // The compact dashboard uses the familiar 0-100% range until a boosted
        // value is configured in the editor. Keep boosted values adjustable
        // instead of collapsing them to 100% on the next interaction.
        const ImGuiID sliderId = ImGui::GetID(id);
        const bool boostedDrag = ImGui::GetActiveID() == sliderId;
        const float maxGain = gain > 1.0001f || boostedDrag ? 4.0f : 1.0f;
        float value = std::clamp(gain, 0.0f, maxGain);
        bool changed = ImGui::SliderFloat(id, &value, 0, maxGain, "", ImGuiSliderFlags_NoInput);
        ImGui::PopStyleVar();
        if (changed)
            gain = value;
        float center = y + 18, end = x + 8 + (width - 16) * std::clamp(gain / maxGain, 0.0f, 1.0f);
        line(x + 8, center, x + width - 8, center, IM_COL32(46, 51, 58, 255), 9);
        line(x + 8, center, end, center, purple, 9);
        dl->AddCircleFilled(p(end, center + 2), 14 * s, IM_COL32(0, 0, 0, 80), 28);
        dl->AddCircleFilled(p(end, center), 13 * s, white, 28);
        if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
            dl->AddCircle(p(end, center), 17 * s, purple, 28, 2 * s);
        char label[24];
        std::snprintf(label, sizeof(label), "%.0f%%", gain * 100);
        text(x + width + 14, y + 4, label, 20);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mix volume: %.0f%% (Windows volume is unchanged)", gain * 100);
        return changed;
    }
};
ImU32 meterColor(float db) {
    return db >= -2    ? IM_COL32(255, 76, 27, 255)
           : db >= -12 ? IM_COL32(243, 224, 25, 255)
                       : IM_COL32(12, 218, 64, 255);
}
void meter(const Canvas &c, float x, float y, float width, float db) {
    for (int i = 0; i < 28; ++i) {
        float threshold = -60.f + 60.f * i / 28;
        c.rect(x + i * width / 28, y, width / 28 - 5, 15,
               db > threshold ? meterColor(threshold) : IM_COL32(48, 54, 61, 255), 0, false);
    }
}
std::string sourceName(const ChannelConfig &c) {
    return !c.label.empty()                    ? c.label
           : c.kind == SourceKind::Application ? "App Audio"
           : c.kind == SourceKind::Microphone  ? "Microphone"
                                               : "Playback Audio";
}
Icon iconFromKey(const std::string &key, Icon fallback) {
    if (key == "microphone")
        return Mic;
    if (key == "speaker")
        return Speaker;
    if (key == "chat")
        return Chat;
    if (key == "headphones")
        return Headphones;
    if (key == "screen")
        return Screen;
    if (key == "wave")
        return Wave;
    return fallback;
}
const char *defaultIconKey(SourceKind kind) {
    return kind == SourceKind::Microphone  ? "microphone"
           : kind == SourceKind::Application ? "chat"
                                             : "speaker";
}
Icon sourceIcon(const ChannelConfig &source, size_t index) {
    const Icon fallback = source.kind == SourceKind::Microphone                  ? Mic
                          : source.kind == SourceKind::Application || index == 1 ? Chat
                                                                                 : Headphones;
    return iconFromKey(source.icon, fallback);
}
ImU32 sourceColor(const ChannelConfig &source, size_t index) {
    const std::string key = source.icon.empty() ? "" : source.icon;
    if (key == "microphone")
        return pink;
    if (key == "chat" || key == "screen")
        return cyan;
    if (key == "wave")
        return green;
    return source.kind == SourceKind::Microphone    ? pink
           : source.kind == SourceKind::Application ? cyan
           : index == 1                             ? cyan
                                                    : purple;
}
bool gainSlider(const Canvas &c, const char *id, float x, float y, float width, float &gain) {
    ImGui::SetCursorScreenPos(c.p(x, y));
    ImGui::SetNextItemWidth(width * c.s);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
    float value = std::clamp(gain, 0.f, 4.f);
    const bool changed = ImGui::SliderFloat(id, &value, 0.f, 4.f, "", ImGuiSliderFlags_NoInput);
    ImGui::PopStyleVar();
    if (changed)
        gain = value;

    const float center = y + 18, end = x + 8 + (width - 16) * std::clamp(gain / 4.f, 0.f, 1.f);
    c.line(x + 8, center, x + width - 8, center, IM_COL32(46, 51, 58, 255), 8);
    c.line(x + 8, center, end, center, purple, 8);
    for (int i = 0; i <= 16; ++i) {
        const float tx = x + 8 + (width - 16) * float(i) / 16.f;
        c.line(tx, center + 15, tx, center + (i % 4 == 0 ? 23 : 19), IM_COL32(54, 60, 68, 255), 1);
    }
    c.dl->AddCircleFilled(c.p(end, center + 2), 13 * c.s, IM_COL32(0, 0, 0, 80), 28);
    c.dl->AddCircleFilled(c.p(end, center), 12 * c.s, white, 28);
    if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
        c.dl->AddCircle(c.p(end, center), 16 * c.s, purple, 28, 2 * c.s);
    return changed;
}
bool gainPercentInput(const Canvas &c, const char *id, float x, float y, float width, float &gain) {
    float percent = std::round(std::clamp(gain, 0.f, 4.f) * 100.f);
    char preview[24];
    std::snprintf(preview, sizeof(preview), "%.0f%%", percent);
    const float horizontalPadding =
        std::max(5.f * c.s, (width * c.s - ImGui::CalcTextSize(preview).x) / 2.f);
    ImGui::SetCursorScreenPos(c.p(x, y));
    ImGui::SetNextItemWidth(width * c.s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {horizontalPadding, ImGui::GetStyle().FramePadding.y});
    const bool changed = ImGui::InputFloat(
        id, &percent, 0.f, 0.f, "%.0f%%",
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    if (changed)
        gain = std::clamp(percent / 100.f, 0.f, 4.f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Type a mix gain from 0%% to 400%%");
    return changed;
}
bool checkboxWithDescription(const Canvas &c, const char *id, float x, float y, float width,
                             bool &value, const char *label, const char *description) {
    constexpr float box = 24.f, gap = 10.f;
    c.rect(x, y, box, box, value ? purple : card, 7, !value);
    if (value)
        c.icon(Check, x + box / 2, y + box / 2, white, 17);
    const float textX = x + box + gap;
    c.text(textX, y + 1, label, 17, white);
    c.wrappedText(textX, y + 29, description, 14, gray,
                  std::max(width - box - gap, 1.f), 42);
    if (c.hit(id, x, y, width, 64)) {
        value = !value;
        return true;
    }
    return false;
}
ImVec4 popupBounds(float scale) {
    const auto *viewport = ImGui::GetMainViewport();
    const float margin = std::ceil(12 * scale);
    return {viewport->WorkPos.x + margin, viewport->WorkPos.y + margin,
            viewport->WorkPos.x + viewport->WorkSize.x - margin,
            viewport->WorkPos.y + viewport->WorkSize.y - margin};
}
void constrainPopupPosition(ImGuiSizeCallbackData *data) {
    // Avoid a one-pixel scrollbar when scaled text produces a fractional height.
    data->DesiredSize.y = std::ceil(data->DesiredSize.y);
    // Size callbacks run during Begin, before the frame and contents are drawn.
    // Preserve a pending centered position on opening; ImGui applies its pivot
    // after calculating the constrained size, including the first measuring frame.
    if (ImGui::GetCurrentWindow()->SetWindowPosVal.x != FLT_MAX)
        return;
    const auto &bounds = *static_cast<const ImVec4 *>(data->UserData);
    const ImVec2 pos{std::clamp(data->Pos.x, bounds.x, std::max(bounds.x, bounds.z - data->DesiredSize.x)),
                     std::clamp(data->Pos.y, bounds.y, std::max(bounds.y, bounds.w - data->DesiredSize.y))};
    if (pos.x != data->Pos.x || pos.y != data->Pos.y)
        ImGui::SetWindowPos(pos);
}
void constrainNextPopup(ImVec4 &bounds, ImVec2 minSize, ImVec2 maxSize) {
    maxSize.x = std::min(maxSize.x, bounds.z - bounds.x);
    maxSize.y = std::floor(std::min(maxSize.y, bounds.w - bounds.y));
    minSize.x = std::min(minSize.x, maxSize.x);
    minSize.y = std::min(minSize.y, maxSize.y);
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize, constrainPopupPosition, &bounds);
}
bool beginBoundedPopup(const char *name, float scale, ImVec2 minSize = {0, 0},
                       ImVec2 maxSize = {FLT_MAX, FLT_MAX}) {
    auto bounds = popupBounds(scale);
    constrainNextPopup(bounds, minSize, maxSize);
    return ImGui::BeginPopup(name, ImGuiWindowFlags_NoMove);
}
bool beginBoundedModal(const char *name, float width, float scale, ImGuiWindowFlags flags = 0) {
    auto bounds = popupBounds(scale);
    ImGui::SetNextWindowSize({width * scale, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    constrainNextPopup(bounds, {width * scale, 0}, {width * scale, FLT_MAX});
    return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | flags);
}
} // namespace
void MixerWindow::init(AudioEngine *engine, Config *config, void *window) {
    engine_ = engine;
    config_ = config;
    window_ = window;
    refreshDevices();
}
void MixerWindow::refreshDevices() {
    DeviceManager devices;
    if (devices.start(nullptr)) {
        playback_ = devices.list(eRender);
        microphones_ = devices.list(eCapture);
    }
    apps_ = listAudioApps();
    refreshTimer_ = 0;
}
void MixerWindow::restart() {
    if (engine_->running()) {
        engine_->stop();
        engine_->start(*config_);
    }
    meters_ = {};
    outputMeter_ = {};
    spectrum_ = Spectrum{};
    lastUnderruns_ = lastDropped_ = 0;
    refreshTimer_ = 2;
}
void MixerWindow::refreshStatus(float dt) {
    refreshTimer_ += dt;
    if (refreshTimer_ >= 2) {
        apps_ = listAudioApps();
        refreshTimer_ = 0;
    }
    severity_ = 0;
    statusDetail_.clear();
    if (!engine_->running()) {
        status_ = "Stopped";
        statusDetail_ = "Monitoring is stopped. Press Start Monitoring to resume your saved mix.";
        return;
    }
    const auto out = engine_->outputStatus();
    uint64_t dropped = 0;
    std::vector<SourceRoute> routes;
    auto warn = [&](const std::string &label, const std::string &detail, int severity = 1) {
        if (severity > severity_) {
            severity_ = severity;
            status_ = label;
        }
        if (!statusDetail_.empty())
            statusDetail_ += "\n\n";
        statusDetail_ += detail;
    };
    status_ = "Optimal";
    if (out.state != StreamState::Running)
        warn("Output unavailable", out.error.empty() ? "Waiting for the selected output device." : out.error,
             2);
    for (size_t i = 0; i < config_->sources.size(); ++i) {
        const auto &source = config_->sources[i];
        const auto state = engine_->channelStatus(static_cast<int>(i));
        dropped += state.dropped;
        SourceRoute route;
        route.kind = source.kind;
        route.endpoint = state.deviceId;
        route.processId = state.processId;
        route.audible = source.enabled && !source.muted && source.gain > 0.0001f &&
                        state.state == StreamState::Running && !config_->output.muted &&
                        config_->output.gain > 0.0001f;
        if (source.enabled && state.state != StreamState::Running)
            warn("Source unavailable",
                 sourceName(source) + ": " + (state.error.empty() ? "Connecting..." : state.error));
        for (const auto &app : apps_)
            if (app.processId == state.processId && state.processId) {
                route.appEndpoints = app.endpoints;
                route.routingKnown = app.routingKnown;
                break;
            }
        if (route.audible && source.kind == SourceKind::Playback && !route.endpoint.empty() &&
            route.endpoint == out.deviceId)
            warn("Feedback risk",
                 sourceName(source) + " captures the selected output device. Choose a different output or "
                                      "disable this source to avoid a feedback loop.",
                 2);
        routes.push_back(std::move(route));
    }
    for (size_t i = 0; i < routes.size(); ++i)
        for (size_t j = i + 1; j < routes.size(); ++j) {
            const auto overlap = sourceOverlap(routes[i], routes[j]);
            if (overlap == Overlap::None)
                continue;
            bool confirmed = overlap == Overlap::Confirmed;
            warn(
                confirmed ? "Duplicate audio" : "Possible duplicate",
                sourceName(config_->sources[i]) + " + " + sourceName(config_->sources[j]) + ": " +
                    (confirmed
                         ? "the same audio is included in both sources. This can cause doubled audio or echo."
                         : "Windows could not confirm the app's playback route. If it plays through this "
                           "device, its audio is captured twice.") +
                    " Mute or disable one source, remove it, or route the app to a different playback "
                    "device.");
            if (confirmed && severity_ == 1)
                status_ = "Duplicate audio";
        }
    if (out.underruns > lastUnderruns_ || dropped > lastDropped_)
        dropoutTimer_ = 4;
    lastUnderruns_ = out.underruns;
    lastDropped_ = dropped;
    dropoutTimer_ = std::max(0.f, dropoutTimer_ - dt);
    if (dropoutTimer_ > 0)
        warn("Audio dropouts", "Audio buffers were missed recently. Try increasing Buffer in Settings.");
    if (clippingTimer_ > 0)
        warn("Clipping", "The mix reached 0 dBFS. Lower a source or master volume to prevent distortion.", 2);
    if (!severity_)
        statusDetail_ = "Audio devices are running. No source overlap or recent dropouts detected.";
}
bool MixerWindow::drawSource(size_t index, float width, float dt) {
    auto &source = config_->sources[index];
    const auto state = engine_->running() ? engine_->channelStatus(static_cast<int>(index)) : ChannelStatus{};
    const auto position = ImGui::GetCursorScreenPos();
    Canvas c{ImGui::GetWindowDrawList(), position, scale_};
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    c.rect(0, 0, width, 214, card, 17);
    auto color = sourceColor(source, index);
    auto icon = sourceIcon(source, index);
    c.badge(icon, 43, 52, color);
    c.scrollingText(90, 23, sourceName(source), 23, white, true, width - 204);
    auto subtitle = state.deviceName.empty() ? toUtf8(source.deviceNameMatch) : toUtf8(state.deviceName);
    if (source.kind == SourceKind::Application)
        subtitle = state.state == StreamState::Running ? "Application audio" : "Waiting for application";
    if (subtitle.empty())
        subtitle = "Choose a device";
    c.scrollingText(90, 61, subtitle, 18, gray, false, width - 204);
    // Keep the full description available on hover without creating a keyboard stop.
    ImGui::SetCursorScreenPos(c.p(90, 59));
    ImGui::Dummy({(width - 204) * scale_, 30 * scale_});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", subtitle.c_str());
    c.rect(width - 107, 30, 34, 36, source.enabled ? purple : IM_COL32(43, 48, 55, 255), 7, false);
    if (source.enabled)
        c.icon(Check, width - 90, 48, white, 26);
    if (c.hit("Enable source", width - 112, 24, 44, 48,
              source.enabled ? "Exclude source from mix" : "Include source in mix")) {
        source.enabled = !source.enabled;
        if (engine_->running())
            engine_->setEnabled(static_cast<int>(index), source.enabled);
        changed = true;
    }
    c.rect(width - 58, 28, 40, 40, IM_COL32(39, 45, 51, 255), 9, false);
    c.icon(Arrow, width - 38, 48, white, 25);
    if (c.hit("Configure source", width - 60, 24, 44, 48, "Configure, rename, or remove this source")) {
        editSource_ = static_cast<int>(index);
        draft_ = source;
        std::snprintf(name_, sizeof(name_), "%s", sourceName(source).c_str());
        openSource_ = true;
        refreshDevices();
    }
    float peak = engine_->running() ? std::max(engine_->channelPeak(static_cast<int>(index)).l.take(),
                                               engine_->channelPeak(static_cast<int>(index)).r.take())
                                    : 0;
    meters_[index].update(peak, dt);
    meter(c, 90, 112, width - 108, meters_[index].levelDb());
    c.icon(source.muted ? Muted : Speaker, 39, 174, source.muted ? red : white, 27);
    if (c.hit("Mute source", 18, 152, 42, 42, source.muted ? "Unmute source" : "Mute source")) {
        source.muted = !source.muted;
        if (engine_->running())
            engine_->setMuted(static_cast<int>(index), source.muted);
        changed = true;
    }
    if (c.volume("##source-volume", 80, 156, width - 161, source.gain)) {
        if (engine_->running())
            engine_->setGain(static_cast<int>(index), source.gain);
        changed = true;
    }
    if (source.enabled && engine_->running() && state.state != StreamState::Running)
        c.dl->AddCircleFilled(c.p(71, 75), 5 * scale_, amber, 16);
    ImGui::SetCursorScreenPos(position);
    ImGui::Dummy({width * scale_, 214 * scale_});
    ImGui::PopID();
    return changed;
}

bool MixerWindow::draw(float dt, int width, int height) {
    dt = std::clamp(dt, .001f, .1f);
    scale_ = std::min(float(width) / 1600.f, float(height) / 986.f);
    const float w = width / scale_, h = height / scale_, rightX = 516, rightW = w - 542, footerY = h - 120,
                outputY = footerY - 288, liveH = outputY - 124;
    bool changed = false;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({float(width), float(height)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushFont(nullptr, 20 * scale_);
    ImGui::Begin("Audio Monitor dashboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    Canvas c{ImGui::GetWindowDrawList(), {0, 0}, scale_};
    c.rect(1, 1, w - 2, h - 2, background, 22);
    c.icon(Wave, 65, 54, purple, 40);
    c.text(109, 36, "Audio Monitor", 31, white, true);
    // The native WM_NCHITTEST handler owns title-bar dragging. An ImGui button
    // here would retain its pressed state when Windows consumes the release.
    const Icon controls[] = {Minus, Maximize, Close};
    const char *tips[] = {"Minimize to tray", "Maximize / restore",
                          config_->closeToTray ? "Close to tray" : "Exit Audio Monitor"};
    for (int i = 0; i < 3; ++i) {
        float x = w - 256 + i * 77.f;
        c.rect(x, 25, 78, 56, card, 13);
        c.icon(controls[i], x + 39, 53, white, 30);
        ImGui::PushID(i + 500);
        if (c.hit("Window control", x, 25, 77, 56, tips[i])) {
            if (i == 0)
                PostMessageW(static_cast<HWND>(window_), WM_SYSCOMMAND, SC_MINIMIZE, 0);
            if (i == 1)
                PostMessageW(static_cast<HWND>(window_), WM_SYSCOMMAND,
                             IsZoomed(static_cast<HWND>(window_)) ? SC_RESTORE : SC_MAXIMIZE, 0);
            if (i == 2)
                PostMessageW(static_cast<HWND>(window_), WM_CLOSE, 0, 0);
        }
        ImGui::PopID();
    }
    c.rect(26, 110, 470, h - 147, panel, 19);
    c.text(56, 140, "Devices", 25, white, true);
    c.icon(Refresh, 452, 153, gray, 31);
    if (c.hit("Refresh devices", 425, 125, 54, 55, "Rescan connected devices and app audio sessions"))
        refreshDevices();
    ImGui::SetCursorScreenPos(c.p(44, 191));
    ImGui::BeginChild("Source list", {434 * scale_, (h - 307) * scale_}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 18 * scale_));
    const float sourceWidth = ImGui::GetContentRegionAvail().x / scale_;
    for (size_t i = 0; i < config_->sources.size(); ++i)
        changed |= drawSource(i, sourceWidth, dt);
    if (config_->sources.empty())
        ImGui::TextWrapped("Add a playback device, microphone, or app to begin your mix.");
    ImGui::PopStyleVar();
    ImGui::EndChild();
    c.centeredIconLabel(Plus, 261.5f, h - 76.5f, "Add Device", 25, 29, purple, purple);
    if (c.hit("Add Device", 74, h - 111, 375, 69, "Add a playback device, microphone, or application")) {
        if (config_->sources.size() < kMaxSources) {
            editSource_ = -1;
            draft_ = ChannelConfig{};
            name_[0] = 0;
            openSource_ = true;
            refreshDevices();
        } else
            ImGui::OpenPopup("Source limit");
    }
    if (beginBoundedPopup("Source limit", scale_)) {
        ImGui::TextUnformatted("Up to 16 sources can be mixed at once.");
        ImGui::EndPopup();
    }
    const auto out = engine_->outputStatus();
    float peak =
        engine_->running() ? std::max(engine_->outputPeak().l.take(), engine_->outputPeak().r.take()) : 0;
    outputMeter_.update(peak, dt);
    clippingTimer_ = peak >= 1.f ? 2.f : std::max(0.f, clippingTimer_ - dt);
    spectrum_.update(engine_->visualSamples(), out.sampleRate, dt,
                     engine_->running() && out.state == StreamState::Running);
    refreshStatus(dt);
    const ImU32 statusColor = !engine_->running() || severity_ >= 2 ? red : severity_ ? amber : green;
    c.rect(rightX, 105, rightW, liveH, panel, 19);
    c.icon(Wave, rightX + 48, 151, white, 31);
    c.text(rightX + 85, 134, "Live Mix", 25, white, true);
    const float graphX = rightX + 32, graphY = 192, graphW = rightW - 107, graphH = liveH - 298;
    c.rect(graphX, graphY, graphW, graphH, IM_COL32(10, 16, 19, 255), 12, false);
    for (int i = 0; i <= 5; ++i) {
        float y = graphY + 18 + (graphH - 30) * float(i) / 5.f;
        c.line(graphX + 18, y, graphX + graphW - 18, y, IM_COL32(26, 34, 39, 255), 1);
        c.text(graphX + graphW + 17, y - 12, std::to_string(-i * 12), 19, gray);
    }
    const float barW = (graphW - 38) / 64.f;
    for (size_t i = 0; i < Spectrum::kBands; ++i) {
        float value = (spectrum_.levels[i] + 60.f) / 60.f, barH = std::max(3.f, value * (graphH - 30));
        float x = graphX + 19 + float(i) * barW, y = graphY + graphH - 13 - barH;
        auto bottom = IM_COL32(0, 217, 57, 255), top = meterColor(spectrum_.levels[i]);
        c.dl->AddRectFilledMultiColor(c.p(x, y), c.p(x + barW * .57f, y + barH), value > .01f ? top : border,
                                      value > .01f ? top : border, value > .01f ? bottom : border,
                                      value > .01f ? bottom : border);
    }
    const float levelY = graphY + graphH + 23;
    c.text(graphX, levelY, "Total Output Level", 23, gray);
    char level[32];
    if (outputMeter_.levelDb() <= -59.9f)
        std::snprintf(level, sizeof(level), "-inf dBFS");
    else
        std::snprintf(level, sizeof(level), "%.1f dBFS", outputMeter_.levelDb());
    c.text(graphX + graphW - 132, levelY, level, 22, peak >= 1 ? red : green, true);
    const float tileY = 105 + liveH - 132, tileW = (rightW - 90) / 4;
    const Icon tileIcons[] = {Wave, Clock, Link, Bars};
    const ImU32 tileColors[] = {green, cyan, purple, statusColor};
    const char *titles[] = {"Sample Rate", "Buffer", "Channels", "Status"};
    std::string rate =
        engine_->running() && out.state == StreamState::Running ? std::to_string(out.sampleRate) : "--";
    if (rate.size() == 5)
        rate.insert(2, ",");
    std::string values[] = {rate + " Hz", std::to_string(config_->bufferMillis) + " ms",
                            config_->mono ? "Mono" : "Stereo", status_};
    for (int i = 0; i < 4; ++i) {
        float x = rightX + 22 + i * (tileW + 15);
        c.rect(x, tileY, tileW, 111, card, 15);
        c.badge(tileIcons[i], x + 48, tileY + 56, tileColors[i]);
        c.text(x + 95, tileY + 26, titles[i], 20, gray);
        c.text(x + 95, tileY + 59, values[i], i == 3 && status_.size() > 14 ? 16.f : 22.f,
               i == 3 ? statusColor : white, false, tileW - 106);
        ImGui::PushID(i + 600);
        if (c.hit("Audio information", x, tileY, tileW, 111,
                  i == 3   ? "View audio health and routing warnings"
                  : i == 2 ? "Switch Stereo / Mono"
                           : "Open audio settings")) {
            if (i == 2)
                openChannels_ = true;
            else if (i == 3)
                openStatus_ = true;
            else {
                settingsPage_ = 0;
                openSettings_ = true;
            }
        }
        ImGui::PopID();
    }
    c.rect(rightX, outputY, rightW, 264, panel, 19);
    c.icon(Link, rightX + 49, outputY + 44, purple, 32);
    c.text(rightX + 82, outputY + 25, "Output Device", 25, white, true);
    c.rect(rightX + 18, outputY + 79, rightW - 38, 164, card, 16);
    const auto outputIcon = iconFromKey(config_->output.icon, Screen);
    // Screen is the output card's default icon and remains purple whether the
    // implicit default or the explicit icon choice is stored.
    const auto outputColor = config_->output.icon.empty() || config_->output.icon == "screen"
                                 ? purple
                                 : sourceColor(config_->output, 0);
    c.badge(outputIcon, rightX + 59, outputY + 127, outputColor);
    auto outputDeviceName = !engine_->running() || out.deviceName.empty()
                                ? toUtf8(config_->output.deviceNameMatch)
                                : toUtf8(out.deviceName);
    if (outputDeviceName.empty())
        outputDeviceName = "Choose output device";
    const auto outputDisplayName = config_->output.label.empty() ? outputDeviceName : config_->output.label;
    c.scrollingText(rightX + 107, outputY + 97, outputDisplayName, 24, white, true, rightW - 318);
    std::string outputSubtitle = engine_->running() && out.state == StreamState::Running
                                     ? (out.exclusive ? "Exclusive audio output" : "Shared audio output")
                                     : "Combined audio destination";
    if (!config_->output.label.empty())
        outputSubtitle = outputDeviceName + " - " + outputSubtitle;
    c.scrollingText(rightX + 107, outputY + 132, outputSubtitle, 19, gray, false, rightW - 318);
    bool active = engine_->running() && out.state == StreamState::Running;
    c.rect(rightX + rightW - 197, outputY + 102, 97, 46,
           active ? IM_COL32(20, 53, 31, 255) : IM_COL32(49, 42, 29, 255), 24, false);
    c.centeredText(rightX + rightW - 148.5f, outputY + 125, active ? "Active" : "Idle", 20,
                   active ? green : amber);
    c.rect(rightX + rightW - 83, outputY + 101, 46, 46, IM_COL32(40, 46, 53, 255), 11, false);
    c.icon(Down, rightX + rightW - 60, outputY + 124, white, 28);
    if (c.hit("Configure output device", rightX + rightW - 86, outputY + 98, 52, 52,
              "Configure the output device, name, icon, and mix gain")) {
        refreshDevices();
        outputDraft_ = config_->output;
        outputDraft_.kind = SourceKind::Playback;
        outputDraft_.processPath.clear();
        outputDraft_.enabled = true;
        std::snprintf(outputName_, sizeof(outputName_), "%s", config_->output.label.c_str());
        openOutput_ = true;
    }
    c.icon(config_->output.muted ? Muted : Speaker, rightX + 54, outputY + 203,
           config_->output.muted ? red : white, 28);
    if (c.hit("Mute output", rightX + 32, outputY + 182, 44, 44,
              config_->output.muted ? "Unmute entire output" : "Mute entire output")) {
        config_->output.muted = !config_->output.muted;
        engine_->setOutputMuted(config_->output.muted);
        changed = true;
    }
    if (c.volume("##master-volume", rightX + 96, outputY + 185, rightW - 221, config_->output.gain)) {
        engine_->setOutputGain(config_->output.gain);
        changed = true;
    }
    const float settingsW = rightW * .238f, stopW = rightW * .35f, stateW = rightW * .325f,
                stopX = rightX + settingsW + (rightW - settingsW - stopW - stateW) * .5f;
    c.rect(rightX, footerY, settingsW, 73, card, 18);
    c.centeredIconLabel(Gear, rightX + settingsW / 2, footerY + 36.5f, "Settings", 25, 34, gray);
    if (c.hit("Settings", rightX, footerY, settingsW, 73)) {
        settingsPage_ = 1;
        openSettings_ = true;
    }
    bool running = engine_->running();
    c.rect(stopX, footerY - 1, stopW, 76, running ? red : IM_COL32(109, 71, 231, 255), 17, false);
    c.centeredIconLabel(running ? Stop : Play, stopX + stopW / 2, footerY + 37,
                        running ? "Stop Monitoring" : "Start Monitoring", 28, 34, white);
    if (c.hit("Toggle monitoring", stopX, footerY, stopW, 74,
              running ? "Stop capturing and sending audio" : "Resume saved mix")) {
        if (running) {
            engine_->updateConfigFromRuntime(*config_);
            engine_->stop();
        } else
            engine_->start(*config_);
        meters_ = {};
        outputMeter_ = {};
        spectrum_ = Spectrum{};
        lastUnderruns_ = lastDropped_ = 0;
        changed = true;
    }
    float stateX = rightX + rightW - stateW;
    c.rect(stateX, footerY, stateW, 73, card, 18);
    c.centeredIconLabel(Dot, stateX + stateW / 2, footerY + 36.5f,
                        running ? "Monitoring Active" : "Monitoring Stopped", 24, 16, running ? green : gray);
    changed |= drawDialogs();
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    return changed;
}

bool MixerWindow::hitTitleBar(int x, int y, int width, int height) const {
    // Let clicks dismiss menus, and keep modal dialogs in charge of input.
    if (!ImGui::GetCurrentContext() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return false;
    const float scale = std::min(float(width) / 1600.f, float(height) / 986.f);
    return x >= 320 * scale && x < width - 270 * scale && y >= 12 * scale && y < 87 * scale;
}

bool MixerWindow::drawDialogs() {
    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24 * scale_, 24 * scale_});
    if (openChannels_) {
        ImGui::OpenPopup("Channels");
        openChannels_ = false;
    }
    if (beginBoundedModal("Channels", 590, scale_,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        constexpr float dialogW = 542.f, dialogH = 390.f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c{ImGui::GetWindowDrawList(), start, scale_};
        ImGui::Dummy({dialogW * scale_, dialogH * scale_});
        ImGui::PushID("channels-dialog");

        c.dl->AddCircleFilled(c.p(32, 30), 31 * scale_, IM_COL32(48, 37, 83, 255), 40);
        c.icon(Link, 32, 30, purple, 34);
        c.text(78, 4, "Channels", 30, white, true);
        c.text(78, 43, "Configure how audio channels are handled", 17, gray);
        c.rect(dialogW - 42, 1, 40, 40, card, 9);
        c.icon(Close, dialogW - 22, 21, white, 25);
        if (c.hit("Close channels", dialogW - 42, 1, 40, 40))
            ImGui::CloseCurrentPopup();

        c.rect(0, 91, dialogW, 202, panel, 14);
        auto drawMode = [&](int mode, float y, const char *title, const char *description) {
            const bool selected = config_->mono == (mode == 1);
            if (selected)
                c.rect(1, y, dialogW - 2, 100, IM_COL32(21, 28, 32, 255), 12, false);
            c.dl->AddCircle(c.p(35, y + 47), 11 * scale_,
                            selected ? purple : IM_COL32(74, 84, 93, 255), 24, 2.5f * scale_);
            if (selected)
                c.dl->AddCircleFilled(c.p(35, y + 47), 5 * scale_, white, 20);
            c.text(72, y + 28, title, 18, white, true, 345);
            c.text(72, y + 63, description, 16, gray, false, 345);

            if (mode == 0) {
                for (int channel = 0; channel < 2; ++channel) {
                    const float x = 458 + channel * 28.f;
                    c.rect(x, y + 25, 19, 45, IM_COL32(38, 44, 51, 255), 5, false);
                    c.rect(x + 7, y + 42, 5, 17, purple, 1, false);
                    c.centeredText(x + 9.5f, y + 83, channel ? "R" : "L", 14, gray);
                }
            } else {
                c.rect(456, y + 25, 48, 45, IM_COL32(38, 44, 51, 255), 7, false);
                c.rect(466, y + 48, 5, 12, gray, 1, false);
                c.rect(477, y + 34, 5, 26, gray, 1, false);
                c.rect(488, y + 44, 5, 16, gray, 1, false);
                c.centeredText(480, y + 83, "L+R", 14, white);
            }

            ImGui::PushID(mode);
            if (c.hit("Channel mode", 0, y, dialogW, 100) && !selected) {
                config_->mono = mode == 1;
                engine_->setMono(config_->mono);
                changed = true;
            }
            ImGui::PopID();
        };
        drawMode(0, 92, "Stereo - separate left and right",
                 "Left and right channels are kept separate.");
        c.line(16, 192, dialogW - 16, 192, border, 1);
        drawMode(1, 193, "Mono - same mix on both sides",
                 "Left and right channels are mixed to mono.");

        c.rect(14, 312, dialogW - 28, 78, card, 11);
        constexpr const char *channelInfo =
            "Changes apply to the output mix and what is sent to your output device.";
        const float channelInfoWidth = dialogW - 108;
        const float channelInfoHeight = c.fontFor(false)
                                            ->CalcTextSizeA(16 * scale_, FLT_MAX,
                                                            channelInfoWidth * scale_, channelInfo)
                                            .y /
                                        scale_;
        c.infoMark(43, 351, purple, 22);
        c.wrappedText(78, 351 - channelInfoHeight / 2, channelInfo, 16, gray,
                      channelInfoWidth, channelInfoHeight + 2);

        ImGui::PopID();
        ImGui::EndPopup();
    }

    if (openStatus_) {
        ImGui::OpenPopup("Status");
        openStatus_ = false;
    }
    if (beginBoundedModal("Status", 620, scale_,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        constexpr float dialogW = 572.f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c{ImGui::GetWindowDrawList(), start, scale_};
        const float detailHeight = std::max(
            c.fontFor(false)->CalcTextSizeA(18 * scale_, FLT_MAX, 456 * scale_, statusDetail_.c_str()).y /
                scale_,
            50.f);
        const float dialogH = 181 + detailHeight;
        ImGui::Dummy({dialogW * scale_, dialogH * scale_});
        ImGui::PushID("status-dialog");
        const ImU32 statusColor = !engine_->running() || severity_ >= 2 ? red : severity_ ? amber : green;

        auto badgeColor = ImGui::ColorConvertU32ToFloat4(statusColor);
        badgeColor.w = .13f;
        c.dl->AddCircleFilled(c.p(32, 30), 31 * scale_,
                              ImGui::ColorConvertFloat4ToU32(badgeColor), 40);
        c.icon(Bars, 32, 30, statusColor, 34);
        c.text(78, 4, "Status", 30, white, true);
        c.text(78, 43, "Overview of your audio monitoring system", 17, gray);
        c.rect(dialogW - 42, 1, 40, 40, card, 9);
        c.icon(Close, dialogW - 22, 21, white, 25);
        if (c.hit("Close status", dialogW - 42, 1, 40, 40))
            ImGui::CloseCurrentPopup();

        c.line(0, 94, dialogW, 94, border, 1);
        c.dl->AddCircle(c.p(36, 139), 16 * scale_, statusColor, 28, 2.5f * scale_);
        c.icon(status_ == "Optimal" ? Check : Bars, 36, 139, statusColor, 23);
        c.text(77, 119, status_, 21, statusColor, true, dialogW - 95);
        c.wrappedText(77, 156, statusDetail_, 18, gray, 456, detailHeight);

        ImGui::PopID();
        ImGui::EndPopup();
    }

    if (openSource_) {
        ImGui::OpenPopup("Configure source");
        openSource_ = false;
    }
    if (beginBoundedModal("Configure source", 770, scale_,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        constexpr float dialogW = 722.f, dialogH = 860.f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c{ImGui::GetWindowDrawList(), start, scale_};
        ImGui::Dummy({dialogW * scale_, dialogH * scale_});
        ImGui::PushID("source-dialog");

        const bool isApp = draft_.kind == SourceKind::Application;
        auto selectKind = [&](bool application) {
            if (application && draft_.kind != SourceKind::Application) {
                draft_.kind = SourceKind::Application;
                draft_.deviceId.clear();
                draft_.deviceNameMatch.clear();
            } else if (!application && draft_.kind == SourceKind::Application) {
                draft_.kind = SourceKind::Playback;
                draft_.processPath.clear();
            }
        };
        auto drawSourceType = [&](const char *id, float x, const char *title, const char *subtitle,
                                  Icon icon, bool selected) {
            c.rect(x, 153, 334, 93, selected ? IM_COL32(22, 27, 33, 255) : card, 10);
            if (selected)
                c.dl->AddRect(c.p(x, 153), c.p(x + 334, 246), purple, 10 * scale_, 0, 2 * scale_);
            c.icon(icon, x + 48, 199, selected ? white : gray, 40);
            c.text(x + 86, 178, title, 19, white, true, 205);
            c.text(x + 86, 209, subtitle, 15, gray, false, 215);
            c.dl->AddCircle(c.p(x + 286, 200), 11 * scale_, selected ? purple : IM_COL32(95, 102, 111, 255),
                            24, 2 * scale_);
            if (selected)
                c.dl->AddCircleFilled(c.p(x + 286, 200), 5 * scale_, white, 16);
            return c.hit(id, x, 153, 334, 93);
        };
        auto drawButton = [&](const char *id, float x, float y, float w, float h, const char *label,
                              Icon icon, ImU32 fill, bool enabled = true) {
            const ImU32 buttonFill = enabled ? fill : IM_COL32(42, 47, 54, 255);
            c.rect(x, y, w, h, buttonFill, 10, false);
            if (icon == Dot)
                c.centeredText(x + w / 2, y + h / 2, label, 19, enabled ? white : gray);
            else
                c.centeredIconLabel(icon, x + w / 2, y + h / 2, label, 21, 27, enabled ? white : gray,
                                    enabled ? white : gray);
            if (!enabled) {
                ImGui::SetCursorScreenPos(c.p(x, y));
                ImGui::InvisibleButton(id, {w * scale_, h * scale_});
                return false;
            }
            return c.hit(id, x, y, w, h);
        };

        c.dl->AddCircleFilled(c.p(32, 30), 31 * scale_, IM_COL32(66, 45, 130, 255), 40);
        c.icon(Plus, 32, 30, white, 33);
        c.text(78, 6, editSource_ < 0 ? "Add Audio Source" : "Edit Audio Source", 31, white, true);
        c.text(78, 45, editSource_ < 0 ? "Select an audio source to monitor and add to the mix"
                                        : "Update this audio source in the mix",
               17, gray);
        c.rect(dialogW - 42, 1, 40, 40, card, 9);
        c.icon(Close, dialogW - 22, 21, white, 25);
        if (c.hit("Close source dialog", dialogW - 42, 1, 40, 40))
            ImGui::CloseCurrentPopup();

        c.rect(0, 91, dialogW, 176, panel, 14);
        c.alignedIconLabel(Wave, 28, 58, 125, "1.  Select Source Type", 20, 28, purple);
        if (drawSourceType("Audio Device", 18, "Audio Device", "Use a headset, speaker, or microphone",
                           Speaker, !isApp))
            selectKind(false);
        if (drawSourceType("Application Audio", 370, "Application Audio", "Capture audio from an application",
                           Screen, isApp))
            selectKind(true);

        c.rect(0, 286, dialogW, 142, panel, 14);
        c.alignedIconLabel(Speaker, 28, 58, 319,
                           isApp ? "2.  Choose Application" : "2.  Choose Device",
                           20, 28, purple);
        std::string selected = "Select a device...";
        if (isApp) {
            for (const auto &app : apps_)
                if (app.path == draft_.processPath) {
                    selected = toUtf8(app.name);
                    break;
                }
            if (selected == "Select a device..." && !draft_.processPath.empty())
                selected = toUtf8(draft_.processPath);
            if (selected == "Select a device...")
                selected = "Select an application...";
        } else if (!draft_.deviceNameMatch.empty())
            selected = toUtf8(draft_.deviceNameMatch);

        ImGui::SetCursorScreenPos(c.p(18, 340));
        ImGui::SetNextItemWidth(542 * scale_);
        if (ImGui::BeginCombo("##Source picker", selected.c_str())) {
            if (isApp) {
                if (apps_.empty())
                    ImGui::TextUnformatted("No application audio sessions found.");
                for (const auto &app : apps_) {
                    const auto label = toUtf8(app.name) + " (" + std::to_string(app.processId) + ")";
                    ImGui::PushID(toUtf8(app.path).c_str());
                    if (ImGui::Selectable(label.c_str(), app.path == draft_.processPath)) {
                        draft_.processPath = app.path;
                        if (!name_[0])
                            std::snprintf(name_, sizeof(name_), "%s", toUtf8(app.name).c_str());
                    }
                    ImGui::PopID();
                }
            } else {
                if (playback_.empty() && microphones_.empty())
                    ImGui::TextUnformatted("No audio devices found.");
                for (const auto &device : playback_) {
                    ImGui::PushID(toUtf8(device.id).c_str());
                    const auto label = toUtf8(device.name) + "  (Playback)";
                    if (ImGui::Selectable(label.c_str(), draft_.kind == SourceKind::Playback &&
                                                         device.id == draft_.deviceId)) {
                        draft_.kind = SourceKind::Playback;
                        draft_.deviceId = device.id;
                        draft_.deviceNameMatch = device.name;
                        if (!name_[0])
                            std::snprintf(name_, sizeof(name_), "%s", toUtf8(device.name).c_str());
                    }
                    ImGui::PopID();
                }
                for (const auto &device : microphones_) {
                    ImGui::PushID(toUtf8(device.id).c_str());
                    const auto label = toUtf8(device.name) + "  (Microphone)";
                    if (ImGui::Selectable(label.c_str(), draft_.kind == SourceKind::Microphone &&
                                                         device.id == draft_.deviceId)) {
                        draft_.kind = SourceKind::Microphone;
                        draft_.deviceId = device.id;
                        draft_.deviceNameMatch = device.name;
                        if (!name_[0])
                            std::snprintf(name_, sizeof(name_), "Microphone");
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        if (drawButton("Refresh source choices", 574, 337, 130, 47, "Refresh", Refresh, card))
            refreshDevices();
        c.text(18, 399,
               isApp ? "Select the application audio session you want to monitor."
                     : "Select the audio device you want to monitor.",
               16, gray, false, dialogW - 36);

        c.rect(0, 448, dialogW, 339, panel, 14);
        c.alignedIconLabel(Sliders, 28, 58, 482, "3.  Customize Source", 20, 27, purple);
        c.text(18, 507, "Icon", 16, white);
        c.line(348, 510, 348, 654, border, 1);
        c.text(372, 507, "Display Name", 16, white);
        char count[24];
        std::snprintf(count, sizeof(count), "%zu / 128", std::strlen(name_));
        c.text(dialogW - 91, 507, count, 15, gray);
        ImGui::SetCursorScreenPos(c.p(372, 531));
        ImGui::SetNextItemWidth(332 * scale_);
        ImGui::InputTextWithHint("##Source name", "e.g. Microphone", name_, sizeof(name_));

        const char *selectedIcon = draft_.icon.empty() ? defaultIconKey(draft_.kind) : draft_.icon.c_str();
        for (int i = 0; i < 6; ++i) {
            const float ix = 18 + float(i % 3) * 55.f, iy = 531 + float(i / 3) * 55.f;
            const bool selectedIconChoice = std::strcmp(selectedIcon, iconChoices[i].key) == 0;
            c.rect(ix, iy, 46, 46, selectedIconChoice ? IM_COL32(39, 30, 67, 255) : card, 7);
            if (selectedIconChoice)
                c.dl->AddRect(c.p(ix, iy), c.p(ix + 46, iy + 46), purple, 7 * scale_, 0, 2 * scale_);
            c.icon(iconChoices[i].icon, ix + 23, iy + 23, selectedIconChoice ? purple : white, 25);
            ImGui::PushID(i + 900);
            if (c.hit("Icon choice", ix, iy, 46, 46))
                draft_.icon = iconChoices[i].key;
            ImGui::PopID();
        }

        checkboxWithDescription(c, "Include in mix", 372, 590, 168, draft_.enabled,
                                "Include in mix", "This source will be mixed into the output.");
        checkboxWithDescription(c, "Source muted", 552, 590, 152, draft_.muted,
                                "Muted", "Start this source muted");

        c.text(18, 671, "Mix Gain", 17, white);
        c.infoMark(107, 680, gray, 19);
        c.icon(Speaker, 28, 711, white, 24);
        float gain = draft_.gain;
        if (gainSlider(c, "##Gain boost", 58, 693, 500, gain))
            draft_.gain = gain;
        gainPercentInput(c, "##Gain percentage", 588, 690, 91, draft_.gain);
        const char *gainMarks[] = {"0%", "100%", "200%", "300%", "400%"};
        for (int i = 0; i < 5; ++i)
            c.centeredText(58 + 500 * float(i) / 4.f, 749, gainMarks[i], 15, gray);
        c.text(18, 780, "Adjust how much of this source is mixed into the output. 100% is normal volume.",
               15, gray, false, dialogW - 36);

        const bool valid = isApp ? processCaptureSupported() && !draft_.processPath.empty()
                                 : !draft_.deviceId.empty() || !draft_.deviceNameMatch.empty();
        if (isApp && !processCaptureSupported())
            c.text(18, 399, "Application capture requires Windows build 20348 or later.", 16, amber,
                   false, dialogW - 36);

        if (editSource_ >= 0 &&
            drawButton("Remove source", 0, 806, 164, 48, "Remove", Close, IM_COL32(61, 40, 86, 255))) {
            config_->sources.erase(config_->sources.begin() + editSource_);
            restart();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        if (drawButton("Cancel source dialog", editSource_ >= 0 ? 386.f : 324.f, 806, 144, 48, "Cancel",
                       Dot, card))
            ImGui::CloseCurrentPopup();
        const float saveX = editSource_ >= 0 ? 546.f : 486.f;
        const float saveW = editSource_ >= 0 ? 176.f : 236.f;
        if (drawButton("Commit source dialog", saveX, 806, saveW, 48,
                       editSource_ < 0 ? "Add Source" : "Save", Plus, IM_COL32(109, 71, 231, 255),
                       valid)) {
            draft_.label = name_[0] ? name_ : sourceName(draft_);
            if (editSource_ < 0)
                config_->sources.push_back(draft_);
            else
                config_->sources[editSource_] = draft_;
            restart();
            changed = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (openOutput_) {
        ImGui::OpenPopup("Configure output");
        openOutput_ = false;
    }
    if (beginBoundedModal("Configure output", 770, scale_,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        constexpr float dialogW = 722.f, dialogH = 658.f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c{ImGui::GetWindowDrawList(), start, scale_};
        ImGui::Dummy({dialogW * scale_, dialogH * scale_});
        ImGui::PushID("output-dialog");

        auto drawButton = [&](const char *id, float x, float y, float w, float h, const char *label,
                              Icon icon, ImU32 fill, bool enabled = true) {
            const ImU32 buttonFill = enabled ? fill : IM_COL32(42, 47, 54, 255);
            c.rect(x, y, w, h, buttonFill, 10, false);
            if (icon == Dot)
                c.centeredText(x + w / 2, y + h / 2, label, 19, enabled ? white : gray);
            else
                c.centeredIconLabel(icon, x + w / 2, y + h / 2, label, 21, 27,
                                    enabled ? white : gray, enabled ? white : gray);
            if (!enabled) {
                ImGui::SetCursorScreenPos(c.p(x, y));
                ImGui::InvisibleButton(id, {w * scale_, h * scale_});
                return false;
            }
            return c.hit(id, x, y, w, h);
        };

        c.dl->AddCircleFilled(c.p(32, 30), 31 * scale_, IM_COL32(66, 45, 130, 255), 40);
        c.icon(Screen, 32, 30, white, 33);
        c.text(78, 6, "Configure Output Device", 31, white, true);
        c.text(78, 45, "Choose where the combined mix is sent and how it appears", 17, gray);
        c.rect(dialogW - 42, 1, 40, 40, card, 9);
        c.icon(Close, dialogW - 22, 21, white, 25);
        if (c.hit("Close output dialog", dialogW - 42, 1, 40, 40))
            ImGui::CloseCurrentPopup();

        c.rect(0, 80, dialogW, 142, panel, 14);
        c.alignedIconLabel(Screen, 28, 58, 113, "1.  Choose Device", 20, 28, purple);
        std::string selected = outputDraft_.deviceNameMatch.empty()
                                   ? "Select an output device..."
                                   : toUtf8(outputDraft_.deviceNameMatch);
        for (const auto &device : playback_)
            if (device.id == outputDraft_.deviceId) {
                selected = toUtf8(device.name);
                break;
            }
        ImGui::SetCursorScreenPos(c.p(18, 134));
        ImGui::SetNextItemWidth(542 * scale_);
        if (ImGui::BeginCombo("##Output picker", selected.c_str())) {
            if (playback_.empty())
                ImGui::TextUnformatted("No playback devices found.");
            for (const auto &device : playback_) {
                ImGui::PushID(toUtf8(device.id).c_str());
                if (ImGui::Selectable(toUtf8(device.name).c_str(), device.id == outputDraft_.deviceId)) {
                    outputDraft_.deviceId = device.id;
                    outputDraft_.deviceNameMatch = device.name;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (drawButton("Refresh output choices", 574, 131, 130, 47, "Refresh", Refresh, card))
            refreshDevices();
        c.text(18, 193, "Select the playback device that will receive the combined audio mix.",
               16, gray, false, dialogW - 36);

        c.rect(0, 240, dialogW, 350, panel, 14);
        c.alignedIconLabel(Sliders, 28, 58, 274, "2.  Customize Output", 20, 27, purple);
        c.text(18, 299, "Icon", 16, white);
        c.line(260, 302, 260, 430, border, 1);
        c.text(286, 299, "Display Name", 16, white);
        char count[24];
        std::snprintf(count, sizeof(count), "%zu / 128", std::strlen(outputName_));
        c.text(dialogW - 91, 299, count, 15, gray);
        ImGui::SetCursorScreenPos(c.p(286, 323));
        ImGui::SetNextItemWidth(418 * scale_);
        ImGui::InputTextWithHint("##Output name", "e.g. Stream Output", outputName_, sizeof(outputName_));

        const char *selectedIcon = outputDraft_.icon.empty() ? "screen" : outputDraft_.icon.c_str();
        for (int i = 0; i < 6; ++i) {
            const float ix = 18 + float(i % 3) * 55.f, iy = 323 + float(i / 3) * 55.f;
            const bool selectedIconChoice = std::strcmp(selectedIcon, iconChoices[i].key) == 0;
            c.rect(ix, iy, 46, 46, selectedIconChoice ? IM_COL32(39, 30, 67, 255) : card, 7);
            if (selectedIconChoice)
                c.dl->AddRect(c.p(ix, iy), c.p(ix + 46, iy + 46), purple, 7 * scale_, 0,
                              2 * scale_);
            c.icon(iconChoices[i].icon, ix + 23, iy + 23,
                   selectedIconChoice ? purple : white, 25);
            ImGui::PushID(i + 1500);
            if (c.hit("Output icon choice", ix, iy, 46, 46))
                outputDraft_.icon = iconChoices[i].key;
            ImGui::PopID();
        }

        checkboxWithDescription(c, "Output muted", 286, 379, 350, outputDraft_.muted,
                                "Muted", "Start the combined output muted");

        c.text(18, 455, "Mix Gain", 17, white);
        c.infoMark(107, 464, gray, 19);
        c.icon(Speaker, 28, 495, white, 24);
        float outputGain = outputDraft_.gain;
        if (gainSlider(c, "##Output gain boost", 58, 477, 500, outputGain))
            outputDraft_.gain = outputGain;
        gainPercentInput(c, "##Output gain percentage", 588, 474, 91, outputDraft_.gain);
        const char *gainMarks[] = {"0%", "100%", "200%", "300%", "400%"};
        for (int i = 0; i < 5; ++i)
            c.centeredText(58 + 500 * float(i) / 4.f, 533, gainMarks[i], 15, gray);
        c.text(18, 559, "Adjust the final output level. 100% is normal volume.",
               15, gray, false, dialogW - 36);

        const bool valid = !outputDraft_.deviceId.empty() || !outputDraft_.deviceNameMatch.empty();
        if (drawButton("Cancel output dialog", 386, 610, 144, 48, "Cancel", Dot, card))
            ImGui::CloseCurrentPopup();
        if (drawButton("Save output dialog", 546, 610, 176, 48, "Save", Check,
                       IM_COL32(109, 71, 231, 255), valid)) {
            const bool deviceChanged = outputDraft_.deviceId != config_->output.deviceId ||
                                       outputDraft_.deviceNameMatch != config_->output.deviceNameMatch;
            outputDraft_.label = outputName_;
            outputDraft_.kind = SourceKind::Playback;
            outputDraft_.processPath.clear();
            outputDraft_.enabled = true;
            config_->output = outputDraft_;
            engine_->setOutputGain(config_->output.gain);
            engine_->setOutputMuted(config_->output.muted);
            if (deviceChanged)
                engine_->setChannelDevice(
                    -1, {config_->output.deviceId, config_->output.deviceNameMatch});
            changed = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (openSettings_) {
        settingsDraft_ = *config_;
        settingsRestoreAll_ = false;
        ImGui::OpenPopup("Settings");
        openSettings_ = false;
    }
    if (beginBoundedModal("Settings", 770, scale_,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        constexpr float dialogW = 722.f, dialogH = 740.f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c{ImGui::GetWindowDrawList(), start, scale_};
        ImGui::Dummy({dialogW * scale_, dialogH * scale_});
        ImGui::PushID("settings-dialog");

        auto drawButton = [&](const char *id, float x, float y, float w, float h, const char *label,
                              ImU32 fill, ImU32 labelColor = white) {
            c.rect(x, y, w, h, fill, 10, false);
            c.centeredText(x + w / 2, y + h / 2, label, 19, labelColor);
            return c.hit(id, x, y, w, h);
        };
        auto drawCheck = [&](const char *id, float x, float y, const char *label, const char *description,
                             bool &value, bool enabled = true) {
            const ImU32 boxColor = value && enabled ? IM_COL32(109, 71, 231, 255)
                                                    : IM_COL32(42, 48, 54, 255);
            c.rect(x, y, 24, 24, boxColor, 5, false);
            if (value)
                c.icon(Check, x + 12, y + 12, enabled ? white : gray, 19);
            c.text(x + 36, y - 2, label, 18, enabled ? white : gray);
            if (description && *description)
                c.text(x + 36, y + 27, description, 14, gray, false, 470);
            if (!enabled)
                return false;
            if (c.hit(id, x - 4, y - 5, 510, description && *description ? 52.f : 34.f)) {
                value = !value;
                return true;
            }
            return false;
        };
        auto drawRadio = [&](const char *id, float x, float y, const char *label, bool selected,
                              bool enabled = true, bool fullCard = false) {
            c.dl->AddCircle(c.p(x, y), 10 * scale_,
                            selected ? purple : IM_COL32(92, 100, 109, 255), 24, 2 * scale_);
            if (selected)
                c.dl->AddCircleFilled(c.p(x, y), 5 * scale_, white, 20);
            c.text(x + 18, y - 10, label, 17, enabled ? white : gray);
            return enabled &&
                   (fullCard ? c.hit(id, x - 24, y - 30, 244, 60)
                             : c.hit(id, x - 12, y - 15, 100, 30));
        };

        c.dl->AddCircleFilled(c.p(32, 30), 31 * scale_, IM_COL32(48, 37, 83, 255), 40);
        c.icon(Gear, 32, 30, purple, 34);
        c.text(78, 6, "Settings", 31, white, true);
        c.rect(dialogW - 42, 1, 40, 40, card, 9);
        c.icon(Close, dialogW - 22, 21, white, 25);
        if (c.hit("Close settings", dialogW - 42, 1, 40, 40))
            ImGui::CloseCurrentPopup();

        struct SettingsNavItem {
            const char *label;
            Icon icon;
            int page;
        };
        constexpr SettingsNavItem nav[] = {{"General", Gear, 1}, {"Audio", Wave, 0},
                                            {"Plugin", Puzzle, 2}, {"About", Info, 3}};
        for (int i = 0; i < 4; ++i) {
            const float y = 92 + i * 58.f;
            const bool selected = settingsPage_ == nav[i].page;
            if (selected)
                c.rect(0, y, 158, 50, IM_COL32(48, 37, 83, 255), 9, false);
            c.icon(nav[i].icon, 24, y + 25, selected ? purple : gray, 27);
            c.text(51, y + 12, nav[i].label, 19, selected ? IM_COL32(185, 147, 255, 255) : white);
            ImGui::PushID(i + 1200);
            if (c.hit("Settings page", 0, y, 158, 50))
                settingsPage_ = nav[i].page;
            ImGui::PopID();
        }

        c.rect(174, 90, 548, 574, panel, 14);
        const float contentX = 196;
        if (settingsPage_ == 0) {
            c.icon(Wave, contentX + 10, 124, white, 27);
            c.text(contentX + 38, 107, "Audio", 21, white, true);

            c.text(contentX, 154, "Output format", 16, gray);
            c.rect(contentX, 181, 504, 76, card, 10);
            c.badge(Wave, contentX + 40, 219, green);
            c.text(contentX + 83, 194, "Sample Rate", 15, gray);
            auto sampleRate = engine_->outputStatus().sampleRate;
            std::string sample = sampleRate ? std::to_string(sampleRate) : "--";
            if (sample.size() == 5)
                sample.insert(2, ",");
            c.text(contentX + 83, 221, sample + " Hz", 19, white);
            c.text(contentX + 264, 194, "Follows the selected output device", 15, gray);
            c.text(contentX + 264, 221, "Change it in Windows Sound settings", 15, gray);

            c.text(contentX, 282, "Buffer", 17, white);
            char bufferLabel[24];
            std::snprintf(bufferLabel, sizeof(bufferLabel), "%u ms", settingsDraft_.bufferMillis);
            c.text(contentX + 443, 282, bufferLabel, 17, purple);
            int buffer = static_cast<int>(settingsDraft_.bufferMillis);
            ImGui::SetCursorScreenPos(c.p(contentX, 309));
            ImGui::SetNextItemWidth(504 * scale_);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
            const bool bufferChanged =
                ImGui::SliderInt("##Settings buffer", &buffer, 20, 250, "", ImGuiSliderFlags_NoInput);
            const bool bufferFocused =
                ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PopStyleVar();
            if (bufferChanged)
                settingsDraft_.bufferMillis = static_cast<uint32_t>(buffer);
            const float bufferFraction = float(buffer - 20) / 230.f;
            c.line(contentX + 8, 327, contentX + 496, 327, IM_COL32(46, 51, 58, 255), 8);
            c.line(contentX + 8, 327, contentX + 8 + 488 * bufferFraction, 327, purple, 8);
            c.dl->AddCircleFilled(c.p(contentX + 8 + 488 * bufferFraction, 327), 11 * scale_, white, 24);
            if (bufferFocused)
                c.dl->AddRect(c.p(contentX, 309), c.p(contentX + 504, 345), purple, 8 * scale_, 0,
                              2 * scale_);
            c.text(contentX, 350, "Lower values reduce delay; higher values better tolerate scheduling jitter.",
                   14, gray, false, 504);

            c.line(contentX, 389, contentX + 504, 389, border, 1);
            c.text(contentX, 410, "Channels", 17, white);
            c.rect(contentX, 440, 244, 60, card, 10);
            c.rect(contentX + 260, 440, 244, 60, card, 10);
            if (drawRadio("Stereo channels", contentX + 24, 470, "Stereo", !settingsDraft_.mono,
                          true, true))
                settingsDraft_.mono = false;
            if (drawRadio("Mono channels", contentX + 284, 470, "Mono", settingsDraft_.mono,
                          true, true))
                settingsDraft_.mono = true;

            c.line(contentX, 521, contentX + 504, 521, border, 1);
            drawCheck("Exclusive output", contentX, 544, "Use exclusive output when available",
                      "System-default outputs remain shared so other apps keep their audio.",
                      settingsDraft_.exclusiveOutput);
        } else if (settingsPage_ == 1) {
            c.icon(Gear, contentX + 10, 124, white, 27);
            c.text(contentX + 38, 107, "General", 21, white, true);
            drawCheck("Start with Windows", contentX, 157, "Start with Windows (in tray)",
                      "Start Audio Monitor automatically when Windows starts.",
                      settingsDraft_.startWithWindows);
            drawCheck("Start hidden", contentX, 219, "Start hidden when launched manually",
                      "Launch Audio Monitor to the system tray.", settingsDraft_.startMinimized);
            drawCheck("Close to tray", contentX, 281, "Close button keeps monitoring in the tray",
                      "Audio monitoring will continue running in the background.",
                      settingsDraft_.closeToTray);

            c.line(contentX, 347, contentX + 504, 347, border, 1);
            c.text(contentX, 369, "Updates", 17, white);
            bool updatesUnavailable = false;
            drawCheck("Automatic updates", contentX, 399, "Check for updates automatically",
                      "Automatic updates are not available in this build.", updatesUnavailable, false);

            c.line(contentX, 463, contentX + 504, 463, border, 1);
            c.text(contentX, 483, "Language", 17, white);
            c.text(contentX + 363, 485, "System Default only", 14, gray);
            c.rect(contentX, 510, 250, 42, card, 8);
            c.text(contentX + 14, 520, "System Default", 17, gray);

            c.line(contentX, 574, contentX + 504, 574, border, 1);
            c.text(contentX, 595, "Theme", 17, white);
            c.text(contentX + 437, 597, "Dark only", 14, gray);
            drawRadio("Dark theme", contentX + 10, 633, "Dark", true, false);
            drawRadio("Light theme", contentX + 120, 633, "Light", false, false);
            drawRadio("System theme", contentX + 230, 633, "System", false, false);
        } else if (settingsPage_ == 2) {
            c.badge(Puzzle, 448, 263, purple);
            c.centeredText(448, 327, "Work In Progress", 28, white);
            c.centeredText(448, 365, "Plugin support is currently being built.", 17, gray);
        } else {
            c.badge(Info, 448, 221, purple);
            c.centeredText(448, 285, "Audio Monitor", 28, white);
            c.centeredText(448, 323, "Version 0.1.0", 17, gray);
            c.centeredText(448, 367, "A low-latency Windows audio monitor and mixer.", 17, gray);
            if (drawButton("Exit application", 350, 421, 196, 48, "Exit Audio Monitor", card))
                exitRequested_ = true;
        }

        if (drawButton("Restore defaults", 0, 691, 166, 49, "Restore Defaults", card,
                       IM_COL32(185, 147, 255, 255)))
            ImGui::OpenPopup("Restore defaults?");
        if (drawButton("Cancel settings", 378, 691, 150, 49, "Cancel", card))
            ImGui::CloseCurrentPopup();
        if (drawButton("Save settings", 544, 691, 178, 49, "Save", IM_COL32(109, 71, 231, 255))) {
            if (settingsDraft_.startWithWindows != config_->startWithWindows &&
                !startup::setEnabled(settingsDraft_.startWithWindows)) {
                ImGui::OpenPopup("Startup setting failed");
            } else {
                const bool exclusiveChanged = settingsDraft_.exclusiveOutput != config_->exclusiveOutput;
                *config_ = settingsDraft_;
                if (settingsRestoreAll_) {
                    restart();
                } else {
                    engine_->setBufferMillis(config_->bufferMillis);
                    engine_->setMono(config_->mono);
                    if (exclusiveChanged) {
                        engine_->setExclusiveOutput(config_->exclusiveOutput);
                        if (engine_->running())
                            engine_->setChannelDevice(
                                -1, {config_->output.deviceId, config_->output.deviceNameMatch});
                    }
                }
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (beginBoundedPopup("Startup setting failed", scale_)) {
            ImGui::TextUnformatted("Could not update Windows startup. Please try again.");
            ImGui::EndPopup();
        }
        if (beginBoundedModal("Restore defaults?", 560, scale_)) {
            ImGui::TextWrapped("Reset all sources, devices, gains, and application preferences?");
            if (ImGui::Button("Reset")) {
                settingsDraft_ = Config::defaults();
                settingsRestoreAll_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return changed;
}
} // namespace audiomon::ui
