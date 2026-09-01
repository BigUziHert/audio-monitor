#include "ui/MixerWindow.h"
#include "ui/Theme.h"
#include "util/Startup.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audiomon::ui {
namespace {

constexpr float kMeterHeight  = 9.0f;
constexpr float kMeterGap     = 3.0f;
constexpr float kFaderMinDb   = -60.0f;
// +12 dB matches the ceiling Config accepts. Without the headroom a config
// holding a boosted gain would display pinned at 0 dB while actually applying
// more, and a quiet microphone could not be brought up at all.
constexpr float kFaderMaxDb   = 12.0f;

ImU32 zoneColour(float db) {
    if (db >= kMeterRedDb)    return IM_COL32(220,  70,  60, 255);
    if (db >= kMeterYellowDb) return IM_COL32(226, 184,  63, 255);
    return IM_COL32(76, 175, 96, 255);
}

// One horizontal bar with OBS's three colour zones, drawn as up to three
// segments so the zone boundaries sit at fixed dB positions rather than
// scaling with the level.
void drawMeterBar(ImDrawList* dl, ImVec2 pos, float width, float height,
                  float levelDb, float holdDb, bool active) {
    const ImU32 bg    = IM_COL32(28, 29, 33, 255);
    const ImU32 track = IM_COL32(44, 46, 52, 255);

    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg, 2.0f);

    if (!active) {
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), track, 2.0f);
        return;
    }

    const float norm = dbToNorm(levelDb);
    if (norm > 0.0f) {
        struct Zone { float startDb, endDb; };
        const Zone zones[3] = {
            { kMeterFloorDb,  kMeterYellowDb },
            { kMeterYellowDb, kMeterRedDb    },
            { kMeterRedDb,    0.0f           },
        };
        for (const Zone& z : zones) {
            const float a = dbToNorm(z.startDb);
            const float b = std::min(dbToNorm(z.endDb), norm);
            if (b <= a) continue;
            dl->AddRectFilled(ImVec2(pos.x + a * width, pos.y),
                              ImVec2(pos.x + b * width, pos.y + height),
                              zoneColour(z.startDb + 0.01f));
        }
    }

    // Peak hold: a bright tick that lingers, so a transient that is already
    // gone is still readable.
    if (holdDb > kMeterFloorDb) {
        const float hx = pos.x + dbToNorm(holdDb) * width;
        dl->AddRectFilled(ImVec2(hx - 1.0f, pos.y), ImVec2(hx + 1.0f, pos.y + height),
                          IM_COL32(240, 240, 245, 230));
    }
}

// dB tick marks under the meters, same landmarks OBS shows.
void drawMeterScale(ImDrawList* dl, ImVec2 pos, float width) {
    const float marks[] = { -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, -5.0f, 0.0f };
    const ImU32 col = IM_COL32(120, 124, 134, 255);
    for (float db : marks) {
        const float x = pos.x + dbToNorm(db) * width;
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + 3.0f), col);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(db));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(x - ts.x * 0.5f, pos.y + 4.0f), col, buf);
    }
}

const char* stateLabel(const ChannelStatus& s) {
    switch (s.state) {
        case StreamState::Running: return s.flowing ? "live" : "quiet";
        case StreamState::Opening: return "opening";
        case StreamState::Failed:  return "unavailable";
        case StreamState::Stopped: return "stopped";
    }
    return "";
}

ImVec4 stateColour(const ChannelStatus& s) {
    switch (s.state) {
        case StreamState::Running: return s.flowing ? ImVec4(0.42f, 0.76f, 0.47f, 1.0f)
                                                    : ImVec4(0.50f, 0.52f, 0.56f, 1.0f);
        case StreamState::Opening: return ImVec4(0.85f, 0.72f, 0.35f, 1.0f);
        default:                   return ImVec4(0.85f, 0.35f, 0.32f, 1.0f);
    }
}

std::string narrow(const std::wstring& w) { return toUtf8(w); }

} // namespace

void MixerWindow::init(AudioEngine* engine, Config* config) {
    engine_ = engine;
    config_ = config;
    strips_[kGame].title = "Game Audio";
    strips_[kChat].title = "Chat Audio";
    strips_[kMic].title  = "Microphone";
    for (int i = 0; i < kChannelCount; ++i) strips_[i].index = i;
}

void MixerWindow::refreshDeviceLists() {
    renderDevices_  = engine_->listDevices(eRender);
    captureDevices_ = engine_->listDevices(eCapture);
    deviceListsLoaded_ = true;
}

bool MixerWindow::drawChannelStrip(Strip& strip, float dt) {
    bool changed = false;
    const int  i  = strip.index;
    const auto st = engine_->channelStatus(i);

    ImGui::PushID(i);
    ImGui::BeginChild("strip", ImVec2(0, 118), true, ImGuiWindowFlags_NoScrollbar);

    // --- header: name, state, device ---
    ImGui::TextUnformatted(strip.title.c_str());
    ImGui::SameLine();
    ImGui::TextColored(stateColour(st), "  %s", stateLabel(st));

    const std::string dev = st.deviceName.empty()
        ? (st.error.empty() ? std::string("(no device)") : st.error)
        : narrow(st.deviceName);
    ImGui::SameLine();
    const float availX = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availX - ImGui::CalcTextSize(dev.c_str()).x - 4.0f));
    ImGui::TextDisabled("%s", dev.c_str());

    // --- meters ---
    // take() returns max-since-last-poll, so nothing is missed between frames.
    strip.meterL.update(engine_->channelPeak(i).l.take(), dt);
    strip.meterR.update(engine_->channelPeak(i).r.take(), dt);

    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const bool   active = st.state == StreamState::Running;

    drawMeterBar(dl, origin, width, kMeterHeight, strip.meterL.levelDb(), strip.meterL.holdDb(), active);
    drawMeterBar(dl, ImVec2(origin.x, origin.y + kMeterHeight + kMeterGap), width, kMeterHeight,
                 strip.meterR.levelDb(), strip.meterR.holdDb(), active);
    drawMeterScale(dl, ImVec2(origin.x, origin.y + 2 * kMeterHeight + kMeterGap + 2.0f), width);
    ImGui::Dummy(ImVec2(width, 2 * kMeterHeight + kMeterGap + 18.0f));

    // --- fader + mute ---
    float gain = config_->game.gain;
    if (i == kChat) gain = config_->chat.gain;
    if (i == kMic)  gain = config_->mic.gain;

    // The fader is linear in dB, which is what makes it feel like a mixer
    // rather than a volume slider crammed into the top 10% of its travel.
    float db = gain > 0.0001f ? linearToDb(gain) : kFaderMinDb;
    db = std::clamp(db, kFaderMinDb, kFaderMaxDb);

    ImGui::SetNextItemWidth(width - 78.0f);
    if (ImGui::SliderFloat("##fader", &db, kFaderMinDb, kFaderMaxDb, "%.1f dB")) {
        const float lin = (db <= kFaderMinDb) ? 0.0f : dbToLinear(db);
        engine_->setGain(i, lin);
        if (i == kGame) config_->game.gain = lin;
        if (i == kChat) config_->chat.gain = lin;
        if (i == kMic)  config_->mic.gain  = lin;
        changed = true;
    }

    bool muted = (i == kGame) ? config_->game.muted
               : (i == kChat) ? config_->chat.muted
                              : config_->mic.muted;

    ImGui::SameLine();
    if (muted) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.24f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.30f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.86f, 0.36f, 0.32f, 1.0f));
    }
    if (ImGui::Button(muted ? "Muted" : "Mute", ImVec2(66, 0))) {
        muted = !muted;
        engine_->setMuted(i, muted);
        if (i == kGame) config_->game.muted = muted;
        if (i == kChat) config_->chat.muted = muted;
        if (i == kMic)  config_->mic.muted  = muted;
        changed = true;
    }
    if (muted) ImGui::PopStyleColor(3);

    ImGui::EndChild();
    ImGui::PopID();
    return changed;
}

bool MixerWindow::drawOutputSection(float dt) {
    bool changed = false;
    const OutputStatus os = engine_->outputStatus();

    ImGui::PushID("out");
    ImGui::BeginChild("outstrip", ImVec2(0, 118), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextUnformatted("Output to capture card");
    ImGui::SameLine();
    if (os.state == StreamState::Running) {
        ImGui::TextColored(ImVec4(0.42f, 0.76f, 0.47f, 1.0f), "  %s  %u Hz",
                           os.exclusive ? "exclusive" : "shared", os.sampleRate);
    } else if (os.state == StreamState::Opening) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.35f, 1.0f), "  connecting");
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.32f, 1.0f), "  waiting for device");
    }

    const std::string dev = os.deviceName.empty()
        ? (os.error.empty() ? std::string("(no device)") : os.error)
        : narrow(os.deviceName);
    ImGui::SameLine();
    const float availX = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availX - ImGui::CalcTextSize(dev.c_str()).x - 4.0f));
    ImGui::TextDisabled("%s", dev.c_str());

    outMeterL_.update(engine_->outputPeak().l.take(), dt);
    outMeterR_.update(engine_->outputPeak().r.take(), dt);

    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const bool   active = os.state == StreamState::Running;

    drawMeterBar(dl, origin, width, kMeterHeight, outMeterL_.levelDb(), outMeterL_.holdDb(), active);
    drawMeterBar(dl, ImVec2(origin.x, origin.y + kMeterHeight + kMeterGap), width, kMeterHeight,
                 outMeterR_.levelDb(), outMeterR_.holdDb(), active);
    drawMeterScale(dl, ImVec2(origin.x, origin.y + 2 * kMeterHeight + kMeterGap + 2.0f), width);
    ImGui::Dummy(ImVec2(width, 2 * kMeterHeight + kMeterGap + 18.0f));

    float db = config_->output.gain > 0.0001f ? linearToDb(config_->output.gain) : kFaderMinDb;
    db = std::clamp(db, kFaderMinDb, kFaderMaxDb);
    ImGui::SetNextItemWidth(width - 78.0f);
    if (ImGui::SliderFloat("##outfader", &db, kFaderMinDb, kFaderMaxDb, "%.1f dB")) {
        const float lin = (db <= kFaderMinDb) ? 0.0f : dbToLinear(db);
        engine_->setOutputGain(lin);
        config_->output.gain = lin;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings", ImVec2(66, 0))) showSettings_ = !showSettings_;

    ImGui::EndChild();
    ImGui::PopID();
    return changed;
}

bool MixerWindow::drawSettings() {
    bool changed = false;
    if (!deviceListsLoaded_) refreshDeviceLists();

    ImGui::BeginChild("settings", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Devices");
    ImGui::Separator();

    struct Row { const char* label; ChannelConfig* cfg; const std::vector<DeviceInfo>* list; int channel; };
    const Row rows[] = {
        { "Game",   &config_->game,   &renderDevices_,  kGame },
        { "Chat",   &config_->chat,   &renderDevices_,  kChat },
        { "Mic",    &config_->mic,    &captureDevices_, kMic  },
        { "Output", &config_->output, &renderDevices_,  -1    },
    };

    for (const Row& row : rows) {
        ImGui::PushID(row.label);
        ImGui::SetNextItemWidth(-90.0f);

        // Show the endpoint that is actually OPEN, not just what was configured.
        // Reporting the name pattern instead is useless precisely when it
        // matters -- when you want to confirm an auto-match landed on the
        // device you meant rather than a similarly named one.
        const std::wstring live = (row.channel >= 0)
            ? engine_->channelStatus(row.channel).deviceName
            : engine_->outputStatus().deviceName;

        std::string current;
        if (!live.empty()) {
            current = narrow(live);
            if (row.cfg->deviceId.empty()) current += "   (auto)";
        } else {
            current = "(nothing matched \"" + toUtf8(row.cfg->deviceNameMatch) + "\")";
        }

        if (ImGui::BeginCombo(row.label, current.c_str())) {
            if (ImGui::Selectable("(auto-detect by name)", row.cfg->deviceId.empty())) {
                row.cfg->deviceId.clear();
                engine_->setChannelDevice(row.channel, { L"", row.cfg->deviceNameMatch });
                changed = true;
            }
            for (const auto& d : *row.list) {
                const bool sel = (d.id == row.cfg->deviceId);
                std::string label = narrow(d.name);
                if (d.isDefault) label += "  [system default]";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    // Narrow the name fallback to what was actually chosen.
                    // Leaving the old broad substring in place means that if
                    // this ID stops resolving, the fallback could silently
                    // pick a different endpoint than the one selected here.
                    row.cfg->deviceId        = d.id;
                    row.cfg->deviceNameMatch = d.name;
                    engine_->setChannelDevice(row.channel, { d.id, d.name });
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    if (ImGui::Button("Rescan devices")) refreshDeviceLists();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Behaviour");
    ImGui::Separator();

    bool autostart = config_->startWithWindows;
    if (ImGui::Checkbox("Start with Windows (minimised to tray)", &autostart)) {
        if (startup::setEnabled(autostart)) { config_->startWithWindows = autostart; changed = true; }
    }

    bool startMin = config_->startMinimized;
    if (ImGui::Checkbox("Start hidden when launched manually", &startMin)) {
        config_->startMinimized = startMin;
        changed = true;
    }

    bool excl = config_->exclusiveOutput;
    if (ImGui::Checkbox("Open the capture card in exclusive mode", &excl)) {
        config_->exclusiveOutput = excl;
        engine_->setExclusiveOutput(excl);   // the engine holds its own copy
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Exclusive mode bypasses the Windows mixer for the lowest latency.\n"
                          "Takes effect the next time the output device is opened.\n"
                          "Automatically refused if the card is a system default device.");
    }

    int buffer = static_cast<int>(config_->bufferMillis);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Buffer (ms)", &buffer, 20, 250)) {
        config_->bufferMillis = static_cast<uint32_t>(buffer);
        engine_->setBufferMillis(config_->bufferMillis);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Target depth for each channel's buffer. Lower is less delay to the\n"
                          "stream PC; higher tolerates more scheduling jitter under game load.\n"
                          "Takes effect the next time the output device is opened.");
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Close settings")) showSettings_ = false;
    ImGui::SameLine();
    if (ImGui::Button("Exit application")) exitRequested_ = true;

    ImGui::EndChild();
    return changed;
}

bool MixerWindow::draw(float dt, int windowW, int windowH) {
    bool changed = false;

    deviceRefreshTimer_ -= dt;
    if (deviceRefreshTimer_ <= 0.0f) { deviceRefreshTimer_ = 5.0f; if (showSettings_) refreshDeviceLists(); }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(float(windowW), float(windowH)));
    ImGui::Begin("audio-monitor", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    for (auto& s : strips_) changed |= drawChannelStrip(s, dt);
    ImGui::Dummy(ImVec2(0, 2));
    changed |= drawOutputSection(dt);

    if (showSettings_) changed |= drawSettings();

    ImGui::End();
    return changed;
}

} // namespace audiomon::ui
