#include "ui/MixerWindow.h"
#include "util/Startup.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

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
    Dot
};
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
    void centeredText(float x, float y, const std::string &label, float size, ImU32 color) const {
        const auto measured = textSize(label, size, false);
        text(x - measured.x / 2, y - measured.y / 2, label, size, color);
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
        if (ImGui::IsItemFocused())
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
    bool volume(const char *id, float x, float y, float width, float &gain) const {
        // Keep the native slider's keyboard navigation; replace its appearance.
        ImGui::SetCursorScreenPos(p(x, y));
        ImGui::SetNextItemWidth(width * s);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
        float value = std::min(gain, 1.0f);
        bool changed = ImGui::SliderFloat(id, &value, 0, 1, "", ImGuiSliderFlags_NoInput);
        ImGui::PopStyleVar();
        if (changed)
            gain = value;
        float center = y + 18, end = x + 8 + (width - 16) * std::min(gain, 1.0f);
        line(x + 8, center, x + width - 8, center, IM_COL32(46, 51, 58, 255), 9);
        line(x + 8, center, end, center, purple, 9);
        dl->AddCircleFilled(p(end, center + 2), 14 * s, IM_COL32(0, 0, 0, 80), 28);
        dl->AddCircleFilled(p(end, center), 13 * s, white, 28);
        if (ImGui::IsItemFocused())
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
    auto color = source.kind == SourceKind::Microphone    ? pink
                 : source.kind == SourceKind::Application ? cyan
                 : index == 1                             ? cyan
                                                          : purple;
    auto icon = source.kind == SourceKind::Microphone                  ? Mic
                : source.kind == SourceKind::Application || index == 1 ? Chat
                                                                       : Headphones;
    c.badge(icon, 43, 52, color);
    c.text(90, 23, sourceName(source), 23, white, true, width - 204);
    auto subtitle = state.deviceName.empty() ? toUtf8(source.deviceNameMatch) : toUtf8(state.deviceName);
    if (source.kind == SourceKind::Application)
        subtitle = state.state == StreamState::Running ? "Application audio" : "Waiting for application";
    if (subtitle.empty())
        subtitle = "Choose a device";
    c.text(90, 61, subtitle, 18, gray, false, width - 108);
    c.hit("Source details", 90, 59, width - 108, 30, subtitle.c_str());
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
    if (ImGui::BeginPopup("Source limit")) {
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
                ImGui::OpenPopup("Mix channels");
            else if (i == 3)
                ImGui::OpenPopup("Audio status");
            else
                openSettings_ = true;
        }
        if (i == 2 && ImGui::BeginPopup("Mix channels")) {
            for (int mode = 0; mode < 2; ++mode)
                if (ImGui::Selectable(mode ? "Mono - same mix on both sides"
                                           : "Stereo - separate left and right",
                                      config_->mono == (mode == 1))) {
                    config_->mono = mode == 1;
                    engine_->setMono(config_->mono);
                    changed = true;
                }
            ImGui::EndPopup();
        }
        if (i == 3) {
            ImGui::SetNextWindowSize({490 * scale_, 0});
            if (ImGui::BeginPopup("Audio status")) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(statusColor), "%s", status_.c_str());
                ImGui::Separator();
                ImGui::TextWrapped("%s", statusDetail_.c_str());
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    c.rect(rightX, outputY, rightW, 264, panel, 19);
    c.icon(Link, rightX + 49, outputY + 44, purple, 32);
    c.text(rightX + 82, outputY + 25, "Output Device", 25, white, true);
    c.rect(rightX + 18, outputY + 79, rightW - 38, 164, card, 16);
    c.badge(Screen, rightX + 59, outputY + 127, purple);
    auto outputName = !engine_->running() || out.deviceName.empty() ? toUtf8(config_->output.deviceNameMatch)
                                                                    : toUtf8(out.deviceName);
    if (outputName.empty())
        outputName = "Choose output device";
    c.text(rightX + 107, outputY + 97, outputName, 24, white, true, rightW - 318);
    std::string outputSubtitle = engine_->running() && out.state == StreamState::Running
                                     ? (out.exclusive ? "Exclusive audio output" : "Shared audio output")
                                     : "Combined audio destination";
    c.text(rightX + 107, outputY + 132, outputSubtitle, 19, gray);
    bool active = engine_->running() && out.state == StreamState::Running;
    c.rect(rightX + rightW - 197, outputY + 102, 97, 46,
           active ? IM_COL32(20, 53, 31, 255) : IM_COL32(49, 42, 29, 255), 24, false);
    c.centeredText(rightX + rightW - 148.5f, outputY + 125, active ? "Active" : "Idle", 20,
                   active ? green : amber);
    c.rect(rightX + rightW - 83, outputY + 101, 46, 46, IM_COL32(40, 46, 53, 255), 11, false);
    c.icon(Down, rightX + rightW - 60, outputY + 124, white, 28);
    if (c.hit("Select output device", rightX + rightW - 86, outputY + 98, 52, 52,
              "Choose where the combined mix is sent")) {
        refreshDevices();
        ImGui::OpenPopup("Output device");
    }
    ImGui::SetNextWindowSizeConstraints({350 * scale_, 0}, {800 * scale_, 450 * scale_});
    if (ImGui::BeginPopup("Output device")) {
        if (playback_.empty())
            ImGui::TextUnformatted("No playback devices found. Connect one and refresh.");
        for (const auto &device : playback_) {
            ImGui::PushID(toUtf8(device.id).c_str());
            if (ImGui::Selectable(toUtf8(device.name).c_str(), device.id == config_->output.deviceId)) {
                config_->output.deviceId = device.id;
                config_->output.deviceNameMatch = device.name;
                engine_->setChannelDevice(-1, {device.id, device.name});
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
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
    if (c.hit("Settings", rightX, footerY, settingsW, 73))
        openSettings_ = true;
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
    if (openSource_) {
        ImGui::OpenPopup("Configure source");
        openSource_ = false;
    }
    ImGui::SetNextWindowSize({650 * scale_, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    if (ImGui::BeginPopupModal("Configure source", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(editSource_ < 0 ? "Add an audio source" : "Source settings");
        ImGui::Separator();
        int kind = static_cast<int>(draft_.kind);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##Source type", &kind, "Playback device\0Microphone\0Application audio\0")) {
            draft_.kind = static_cast<SourceKind>(kind);
            draft_.deviceId.clear();
            draft_.deviceNameMatch.clear();
            draft_.processPath.clear();
        }
        bool valid = false;
        if (draft_.kind == SourceKind::Application) {
            if (!processCaptureSupported())
                ImGui::TextWrapped("Application capture requires Windows build 20348 or later (Windows 11 "
                                   "recommended) and a recent Windows SDK.");
            ImGui::TextWrapped("Captures this app and its child processes. Start audio in the app, then "
                               "Refresh if it is missing.");
            std::string selected =
                draft_.processPath.empty() ? "Choose an application" : toUtf8(draft_.processPath);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Application", selected.c_str())) {
                for (const auto &app : apps_) {
                    auto label = toUtf8(app.name) + " (" + std::to_string(app.processId) + ")";
                    if (ImGui::Selectable(label.c_str(), app.path == draft_.processPath)) {
                        draft_.processPath = app.path;
                        if (!name_[0])
                            std::snprintf(name_, sizeof(name_), "%s", toUtf8(app.name).c_str());
                    }
                }
                ImGui::EndCombo();
            }
            valid = processCaptureSupported() && !draft_.processPath.empty();
        } else {
            const auto &devices = draft_.kind == SourceKind::Microphone ? microphones_ : playback_;
            std::string selected = toUtf8(draft_.deviceNameMatch);
            if (selected.empty())
                selected = "Choose an audio device";
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Device", selected.c_str())) {
                for (const auto &device : devices) {
                    ImGui::PushID(toUtf8(device.id).c_str());
                    if (ImGui::Selectable(toUtf8(device.name).c_str(), device.id == draft_.deviceId)) {
                        draft_.deviceId = device.id;
                        draft_.deviceNameMatch = device.name;
                        if (!name_[0])
                            std::snprintf(name_, sizeof(name_), "%s",
                                          draft_.kind == SourceKind::Microphone
                                              ? "Microphone"
                                              : toUtf8(device.name).c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            valid = !draft_.deviceId.empty() || !draft_.deviceNameMatch.empty();
        }
        if (ImGui::Button("Refresh"))
            refreshDevices();
        ImGui::TextUnformatted("Display name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##Source name", name_, sizeof(name_));
        ImGui::Checkbox("Include in mix", &draft_.enabled);
        ImGui::SameLine();
        ImGui::Checkbox("Muted", &draft_.muted);
        float gain = draft_.gain * 100;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##Gain boost", &gain, 0, 400, "Mix gain %.0f%%"))
            draft_.gain = gain / 100;
        ImGui::TextDisabled("Above 100%% boosts quiet sources. Watch for clipping.");
        ImGui::Separator();
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(editSource_ < 0 ? "Add source" : "Save changes")) {
            draft_.label = name_[0] ? name_ : sourceName(draft_);
            if (editSource_ < 0)
                config_->sources.push_back(draft_);
            else
                config_->sources[editSource_] = draft_;
            restart();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        if (editSource_ >= 0) {
            ImGui::SameLine();
            if (ImGui::Button("Remove source")) {
                config_->sources.erase(config_->sources.begin() + editSource_);
                restart();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (openSettings_) {
        ImGui::OpenPopup("Settings");
        openSettings_ = false;
    }
    ImGui::SetNextWindowSize({660 * scale_, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Audio");
        ImGui::Separator();
        ImGui::Text("Sample rate: %u Hz", engine_->outputStatus().sampleRate);
        ImGui::TextWrapped("The sample rate follows the output device. Configure its supported format in "
                           "Windows Sound settings.");
        int buffer = static_cast<int>(config_->bufferMillis);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##Buffer", &buffer, 20, 250, "Buffer: %d ms")) {
            config_->bufferMillis = static_cast<uint32_t>(buffer);
            engine_->setBufferMillis(config_->bufferMillis);
            changed = true;
        }
        ImGui::TextWrapped(
            "Lower buffers reduce delay; higher buffers tolerate scheduling jitter. Applies live.");
        int mono = config_->mono ? 1 : 0;
        if (ImGui::Combo("Channels", &mono, "Stereo\0Mono\0")) {
            config_->mono = mono == 1;
            engine_->setMono(config_->mono);
            changed = true;
        }
        if (ImGui::Checkbox("Use exclusive output when available", &config_->exclusiveOutput)) {
            engine_->setExclusiveOutput(config_->exclusiveOutput);
            if (engine_->running())
                engine_->setChannelDevice(-1, {config_->output.deviceId, config_->output.deviceNameMatch});
            changed = true;
        }
        ImGui::TextWrapped("System default outputs always use shared mode so other apps keep their audio.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Application");
        ImGui::Separator();
        bool autostart = config_->startWithWindows;
        if (ImGui::Checkbox("Start with Windows (in tray)", &autostart)) {
            if (startup::setEnabled(autostart)) {
                config_->startWithWindows = autostart;
                changed = true;
            } else
                ImGui::OpenPopup("Startup setting failed");
        }
        if (ImGui::BeginPopup("Startup setting failed")) {
            ImGui::TextUnformatted("Could not update Windows startup. Please try again.");
            ImGui::EndPopup();
        }
        changed |= ImGui::Checkbox("Start hidden when launched manually", &config_->startMinimized);
        changed |= ImGui::Checkbox("Close button keeps monitoring in the tray", &config_->closeToTray);
        ImGui::TextDisabled("Minimize always sends the app to the tray.");
        ImGui::Separator();
        if (ImGui::Button("Done"))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Restore defaults"))
            ImGui::OpenPopup("Restore defaults?");
        ImGui::SameLine();
        if (ImGui::Button("Exit application"))
            exitRequested_ = true;
        if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Reset all sources, devices, gains, and application preferences?");
            if (ImGui::Button("Reset")) {
                bool startupChanged = startup::setEnabled(false), oldStartup = config_->startWithWindows;
                *config_ = Config::defaults();
                if (!startupChanged)
                    config_->startWithWindows = oldStartup;
                restart();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return changed;
}
} // namespace audiomon::ui
