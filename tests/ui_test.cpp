// Exercise the actual ImGui dashboard without opening a desktop window or audio
// devices. This catches layout bounds and popup/control regressions headlessly.
#include "ui/MixerWindow.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
using namespace audiomon;

namespace audiomon {
// Supply a running meter session without opening hardware or worker threads.
// The UI still consumes the engine's real peak hand-off and toggle API.
struct AudioEngineTestAccess {
    static void prepareMeterSession(AudioEngine &engine, const Config &config) {
        engine.config_ = config;
        engine.sourceCount_ = config.sources.size();
        engine.outputCount_ = config.outputCount();
        engine.running_.store(true);
        engine.monitoringState_.store(2u);
    }
    static void setMonitoringState(AudioEngine &engine, bool monitoring) {
        engine.monitoringState_.store(monitoring ? 3u : 2u);
    }
    static bool sourceMuted(const AudioEngine &engine, size_t index) {
        return engine.channels_[index]->muted.load();
    }
    static bool outputMuted(const AudioEngine &engine, size_t index) {
        return engine.outputMuted_[index].load();
    }
};
} // namespace audiomon

namespace audiomon::ui {
struct MixerWindowTestAccess {
    static void resetDropoutState(MixerWindow &mixer) {
        mixer.status_.clear();
        mixer.statusDetail_.clear();
        mixer.severity_ = 0;
        mixer.dropoutTimer_ = 0;
        mixer.dropoutBaselineValid_ = false;
        mixer.lastUnderruns_ = 0;
        mixer.lastDropped_ = 0;
    }
    static void refreshDropoutStatus(MixerWindow &mixer, StreamState outputState,
                                     uint64_t underruns, uint64_t dropped, float dt = 1.f / 60.f) {
        mixer.statusDetail_.clear();
        mixer.severity_ = 0;
        mixer.refreshDropoutStatus(outputState, underruns, dropped, dt);
    }
    static bool dropoutTrackingReset(const MixerWindow &mixer) {
        return !mixer.dropoutBaselineValid_ && mixer.dropoutTimer_ == 0.f;
    }
    static bool updateStartupSettling(MixerWindow &mixer, bool running,
                                      bool allStreamsReady, float dt) {
        return mixer.updateStartupSettling(running, allStreamsReady, dt);
    }
    static const std::string &statusDetail(const MixerWindow &mixer) {
        return mixer.statusDetail_;
    }
    static void makeSourceDraftValid(MixerWindow &mixer) {
        mixer.draft_.kind = SourceKind::Playback;
        mixer.draft_.deviceId = L"test-device-id";
        mixer.draft_.deviceNameMatch = L"Test Device";
    }
    static int settingsPage(const MixerWindow &mixer) {
        return mixer.settingsPage_;
    }
    static Config &settingsDraft(MixerWindow &mixer) { return mixer.settingsDraft_; }
    static const Keybind &capturedKeybind(const MixerWindow &mixer) {
        return mixer.capturedKeybind_;
    }
    static int keybindTarget(const MixerWindow &mixer) { return mixer.keybindTarget_; }
    static void captureKeybind(MixerWindow &mixer, uint32_t key, uint32_t modifiers,
                               bool repeated = false) {
        mixer.captureKeybind(key, modifiers, repeated);
    }
    static bool executeHotkeyAction(MixerWindow &mixer, Hotkeys::ActionKind kind,
                                    size_t index = 0) {
        return mixer.executeHotkeyAction({kind, index});
    }
    static void setPlaybackDevices(MixerWindow &mixer,
                                   const std::vector<DeviceInfo> &devices) {
        mixer.playback_ = devices;
    }
    static const ChannelConfig &outputDraft(const MixerWindow &mixer) {
        return mixer.outputDraft_;
    }
    static void setOutputDraftDevice(MixerWindow &mixer, const std::wstring &id,
                                     const std::wstring &name) {
        mixer.outputDraft_.deviceId = id;
        mixer.outputDraft_.deviceNameMatch = name;
    }
    static size_t selectedOutput(const MixerWindow &mixer) {
        return mixer.selectedOutput_;
    }
    static void setSelectedOutput(MixerWindow &mixer, size_t output) {
        mixer.selectedOutput_ = output;
    }
    static float sourceMeterLevelDb(const MixerWindow &mixer, size_t source) {
        return mixer.meters_.at(source).levelDb();
    }
    static void resetSourceMeters(MixerWindow &mixer) { mixer.meters_ = {}; }
    static uint64_t statusEvaluationCount(const MixerWindow &mixer) {
        return mixer.statusEvaluationCount_;
    }
    static uint64_t spectrumEvaluationCount(const MixerWindow &mixer) {
        return mixer.spectrumEvaluationCount_;
    }
    static void forceExpensiveRefresh(MixerWindow &mixer) {
        mixer.statusRefreshForced_ = true;
        mixer.spectrumWasActive_ = false;
    }
    static void resetPerformanceCadence(MixerWindow &mixer) {
        mixer.statusRefreshTimer_ = 0;
        mixer.spectrumRefreshTimer_ = 0;
        mixer.statusRefreshForced_ = true;
        mixer.spectrumWasActive_ = false;
    }
    static void setExportFuture(MixerWindow &mixer, std::future<DiagnosticExportResult> future) {
        mixer.diagnosticExport_ = std::move(future);
    }
    static bool exportPending(const MixerWindow &mixer) { return mixer.diagnosticExport_.valid(); }
    static const std::wstring &exportPath(const MixerWindow &mixer) { return mixer.diagnosticPath_; }
    static bool exportFailed(const MixerWindow &mixer) { return mixer.diagnosticExportFailed_; }
    static void startExport(MixerWindow &mixer, const std::wstring &directory) {
        mixer.startDiagnosticExport(directory);
    }
};
} // namespace audiomon::ui

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failed = 0;
    {
        AudioEngine engine;
        Config config = Config::defaults();
        // Most interaction fixtures exercise an existing primary output.
        // First-run production defaults intentionally contain no outputs.
        ChannelConfig initialOutput;
        initialOutput.deviceNameMatch = L"Elgato 4K";
        config.addOutput(initialOutput);
        ui::MixerWindow mixer;
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        unsigned char *pixels;
        int fw, fh;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
        ui::applyTheme();
        mixer.init(&engine, &config, nullptr);
        auto expect = [&](bool condition, const char *message) {
            if (!condition) {
                std::printf("%s at frame %d\n", message, ImGui::GetFrameCount());
                ++failed;
            }
        };
        expect(config.monitoringKeybind == Keybind{} &&
                   config.output.muteKeybind == Keybind{} &&
                   std::all_of(config.sources.begin(), config.sources.end(),
                               [](const ChannelConfig &source) {
                                   return source.muteKeybind == Keybind{};
                               }),
               "Initial monitoring or device keybind was not blank");
        auto popup = []() -> ImGuiWindow * {
            const auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
            return stack.empty() ? nullptr : stack.back().Window;
        };
        auto expectFixedModal = [&](ImGuiWindow *dialog, const char *label) {
            expect(dialog && (dialog->Flags & ImGuiWindowFlags_Modal), label);
            expect(dialog && (dialog->Flags & ImGuiWindowFlags_NoMove),
                   "Custom dialog can be dragged");
            expect(dialog && (dialog->Flags & ImGuiWindowFlags_NoTitleBar),
                   "Custom dialog has a native title bar");
        };
        int changedFrames = 0;
        auto frame = [&](int w, int h) {
            io.DisplaySize = {float(w), float(h)};
            io.DeltaTime = 1.f / 60;
            ImGui::NewFrame();
            if (mixer.draw(io.DeltaTime, w, h)) ++changedFrames;
            ImGui::Render();
            if (!ImGui::GetDrawData() || ImGui::GetDrawData()->TotalVtxCount == 0) {
                std::printf("No draw data at %dx%d frame %d\n",w,h,ImGui::GetFrameCount()); ++failed;
            }
        };

        if (GetEnvironmentVariableA("AUDIOMON_UI_BENCHMARK", nullptr, 0) != 0) {
            config.sources.resize(kMaxSources);
            for (size_t i = 0; i < config.sources.size(); ++i) {
                config.sources[i].label = "Synthetic source " + std::to_string(i + 1);
                config.sources[i].deviceNameMatch = L"Synthetic playback endpoint";
            }
            config.additionalOutputs.resize(kMaxOutputs - 1);
            for (size_t i = 0; i < config.additionalOutputs.size(); ++i) {
                config.additionalOutputs[i].label = "Synthetic output " + std::to_string(i + 2);
                config.additionalOutputs[i].deviceId = L"synthetic-output-" + std::to_wstring(i + 2);
            }
            AudioEngineTestAccess::prepareMeterSession(engine, config);
            AudioEngineTestAccess::setMonitoringState(engine, true);
            mixer.setVisible(true);
            auto benchmark = [&](const char *label, bool forceEveryFrame) {
                ui::MixerWindowTestAccess::resetPerformanceCadence(mixer);
                const auto started = std::chrono::steady_clock::now();
                for (int rendered = 0; rendered < 600; ++rendered) {
                    if (forceEveryFrame)
                        ui::MixerWindowTestAccess::forceExpensiveRefresh(mixer);
                    for (int source = 0; source < kMaxSources; ++source)
                        engine.channelPeak(source).l.publish(.35f);
                    auto &visual = engine.visualSamples();
                    const uint32_t samples = std::min<uint32_t>(480, visual.beginWrite());
                    for (uint32_t sample = 0; sample < samples; ++sample)
                        visual.writeFrame(sample, .25f, -.25f);
                    visual.endWrite(samples);
                    frame(1600, 986);
                }
                const auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                std::printf("Synthetic dashboard (%s): 600 frames, %.3f ms, %.3f ms/frame\n",
                            label, elapsed, elapsed / 600.0);
            };
            benchmark("per-frame status + FFT", true);
            benchmark("bounded status + FFT", false);
            ImGui::DestroyContext();
            engine.stop();
            CoUninitialize();
            return 0;
        }

        // Paused and forwarding source cards must consume the same engine
        // peaks with identical attack/release. No hardware is opened here.
        AudioEngineTestAccess::prepareMeterSession(engine, config);
        mixer.setVisible(true);
        for (bool monitoring : {false, true}) {
            AudioEngineTestAccess::setMonitoringState(engine, monitoring);
            ui::MixerWindowTestAccess::resetSourceMeters(mixer);
            MeterBallistics expectedMeter;
            for (float peak : {.02f, .2f, .8f, .4f, 0.f, 0.f}) {
                engine.channelPeak(0).l.publish(peak * .75f);
                engine.channelPeak(0).r.publish(peak);
                expectedMeter.update(peak, 1.f / 60.f);
                frame(1600, 986);
                expect(std::abs(ui::MixerWindowTestAccess::sourceMeterLevelDb(mixer, 0) -
                                expectedMeter.levelDb()) < .001f,
                       "Source meter response differed between paused and forwarding audio");
            }
        }

        // Expensive health/routing reconstruction and FFT work must be tied to
        // human-visible update rates, not a 144/240 Hz desktop refresh rate.
        const uint64_t statusBefore =
            ui::MixerWindowTestAccess::statusEvaluationCount(mixer);
        const uint64_t spectrumBefore =
            ui::MixerWindowTestAccess::spectrumEvaluationCount(mixer);
        for (int rendered = 0; rendered < 120; ++rendered) {
            auto &visual = engine.visualSamples();
            const uint32_t samples = std::min<uint32_t>(480, visual.beginWrite());
            for (uint32_t sample = 0; sample < samples; ++sample)
                visual.writeFrame(sample, .2f, -.2f);
            visual.endWrite(samples);
            frame(1600, 986);
        }
        const uint64_t statusUpdates =
            ui::MixerWindowTestAccess::statusEvaluationCount(mixer) - statusBefore;
        const uint64_t spectrumUpdates =
            ui::MixerWindowTestAccess::spectrumEvaluationCount(mixer) - spectrumBefore;
        expect(statusUpdates >= 9 && statusUpdates <= 11,
               "Status work was not capped near 5 Hz");
        expect(spectrumUpdates >= 58 && spectrumUpdates <= 61,
               "Spectrum work was not capped near 30 Hz");

        // Exercise the actual Start/Stop Monitoring button, including its
        // release frame: a reset there visibly freezes or blanks the meter.
        AudioEngineTestAccess::setMonitoringState(engine, false);
        {
            auto &visual = engine.visualSamples();
            const uint32_t silentFrames = visual.beginWrite();
            for (uint32_t sample = 0; sample < silentFrames; ++sample)
                visual.writeFrame(sample, 0.f, 0.f);
            visual.endWrite(silentFrames);
            const uint64_t beforePausedFrame =
                ui::MixerWindowTestAccess::spectrumEvaluationCount(mixer);
            frame(1600, 986);
            expect(visual.depth() == 0 &&
                       ui::MixerWindowTestAccess::spectrumEvaluationCount(mixer) == beforePausedFrame,
                   "Paused dashboard copied silent samples through the FFT");
        }
        ui::MixerWindowTestAccess::resetSourceMeters(mixer);
        MeterBallistics continuousMeter;
        auto meterFrame = [&](float peak) {
            engine.channelPeak(0).l.publish(peak);
            continuousMeter.update(peak, 1.f / 60.f);
            frame(1600, 986);
            expect(std::abs(ui::MixerWindowTestAccess::sourceMeterLevelDb(mixer, 0) -
                            continuousMeter.levelDb()) < .001f,
                   "Toggling monitoring reset or interrupted the source meter ballistics");
        };
        meterFrame(.8f);
        io.AddMousePosEvent(1000, 900);
        meterFrame(0.f);
        for (bool monitoring : {true, false}) {
            io.AddMouseButtonEvent(0, true);
            meterFrame(0.f);
            io.AddMouseButtonEvent(0, false);
            meterFrame(0.f);
            expect(engine.running() && engine.monitoring() == monitoring,
                   "Monitoring toggle stopped the shared capture session");
        }
        mixer.setVisible(false);
        expect(!engine.running() && !engine.monitoring(),
               "Hiding the paused dashboard left its capture session running");
        ui::MixerWindowTestAccess::resetSourceMeters(mixer);

        ui::MixerWindowTestAccess::resetDropoutState(mixer);
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Stopped, 0, 1);
        expect(ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") ==
                   std::string::npos,
               "Stopped output attributed a dropped capture frame to an audio dropout");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 0, 2);
        expect(ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") !=
                   std::string::npos,
               "Running output did not report a newly dropped capture frame");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Stopped, 0, 2);
        expect(ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") ==
                   std::string::npos,
               "Stopped output retained a recent dropout warning");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 0, 3);
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 0, 3, 2.f);
        expect(ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") !=
                   std::string::npos,
               "Recent dropout warning expired before its display interval");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 0, 3, 2.f);
        expect(ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") ==
                   std::string::npos,
               "Unchanged dropout totals kept renewing an expired warning");

        // Cumulative counters can already be nonzero when the dashboard is
        // restored from the tray. Its first snapshot establishes a baseline;
        // only an increase observed afterward represents a recent dropout.
        ui::MixerWindowTestAccess::resetDropoutState(mixer);
        auto hasDropoutWarning = [&] {
            return ui::MixerWindowTestAccess::statusDetail(mixer).find("Audio dropouts") !=
                   std::string::npos;
        };
        mixer.setVisible(true);
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 14, 27);
        expect(!hasDropoutWarning(), "Initial historic counters were reported as recent dropouts");
        mixer.setVisible(true); // Repeating the current visibility must keep the baseline.
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 14, 28);
        expect(hasDropoutWarning(), "A new dropout after the initial baseline was ignored");
        mixer.setVisible(false);
        expect(ui::MixerWindowTestAccess::dropoutTrackingReset(mixer),
               "Hiding the dashboard retained its dropout baseline or warning timer");
        mixer.setVisible(true);
        expect(ui::MixerWindowTestAccess::dropoutTrackingReset(mixer),
               "Restoring the dashboard reused its previous dropout baseline or timer");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 31, 62);
        expect(!hasDropoutWarning(), "Tray restore replayed dropouts accumulated while hidden");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 32, 62);
        expect(hasDropoutWarning(), "A fresh underrun after tray restore was ignored");

        // Stop/Start Monitoring preserves the metering session and its historic
        // totals, but must invalidate the old warning's observation baseline.
        AudioEngineTestAccess::prepareMeterSession(engine, config);
        AudioEngineTestAccess::setMonitoringState(engine, true);
        expect(ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::Monitoring) &&
                   engine.running() && !engine.monitoring() &&
                   ui::MixerWindowTestAccess::dropoutTrackingReset(mixer),
               "Stop Monitoring retained an active dropout warning or stopped source metering");
        expect(ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::Monitoring) &&
                   engine.running() && engine.monitoring() &&
                   ui::MixerWindowTestAccess::dropoutTrackingReset(mixer),
               "Start Monitoring reused an old dropout observation baseline");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 32, 62);
        expect(!hasDropoutWarning(), "Stop/Start Monitoring replayed historic dropout totals");
        ui::MixerWindowTestAccess::refreshDropoutStatus(
            mixer, StreamState::Running, 32, 63);
        expect(hasDropoutWarning(), "A new dropout after Stop/Start Monitoring was ignored");
        AudioEngineTestAccess::setMonitoringState(engine, false);
        mixer.setVisible(false);
        ui::MixerWindowTestAccess::resetDropoutState(mixer);

        expect(!ui::MixerWindowTestAccess::updateStartupSettling(mixer, false, false, .1f),
               "Stopped monitoring entered the startup grace period");
        expect(ui::MixerWindowTestAccess::updateStartupSettling(mixer, true, false, .1f),
               "A newly started stream did not suppress transient availability warnings");
        expect(!ui::MixerWindowTestAccess::updateStartupSettling(mixer, true, true, .1f),
               "Ready streams did not end the startup grace period immediately");
        ui::MixerWindowTestAccess::updateStartupSettling(mixer, false, false, .1f);
        expect(ui::MixerWindowTestAccess::updateStartupSettling(mixer, true, false, .5f) &&
                   ui::MixerWindowTestAccess::updateStartupSettling(mixer, true, false, .5f) &&
                   !ui::MixerWindowTestAccess::updateStartupSettling(mixer, true, false, .51f),
               "Startup grace did not expire for a persistently unavailable stream");
        ui::MixerWindowTestAccess::updateStartupSettling(mixer, false, false, .1f);

        io.DisplaySize = {1600, 986};
        io.DeltaTime = 1.f / 60;
        ImGui::NewFrame();
        expect(!mixer.draw(io.DeltaTime, 0, 0), "Zero-size dashboard reported a change");
        ImGui::Render();
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            expect(std::isfinite(window->Size.x) && std::isfinite(window->Size.y),
                   "Zero-size dashboard produced a non-finite window size");
        expect(ImGui::GetColorU32(ImGuiCol_NavCursor) == ui::themePalette().accent,
               "Native controls do not have a visible theme-colored keyboard focus ring");

        for (auto size : {ImVec2(1600, 986), ImVec2(1440, 890), ImVec2(960, 600), ImVec2(2200, 1200)}) {
            frame(int(size.x), int(size.y));
            frame(int(size.x), int(size.y));
            const float scale = std::min(size.x / 1600.f, size.y / 986.f);
            expect(mixer.hitTitleBar(int(700 * scale), int(50 * scale), int(size.x), int(size.y)),
                   "Title bar does not route dragging to Windows");
            expect(mixer.hitTitleBar(int(100 * scale), int(50 * scale), int(size.x), int(size.y)),
                   "Application title and logo cannot be used to move the main window");
            expect(mixer.hitTitleBar(int(size.x - 205 * scale), int(50 * scale),
                                    int(size.x), int(size.y)),
                   "Compact window controls left an unnecessary gap in the draggable title bar");
            for (int button = 0; button < 3; ++button)
                expect(!mixer.hitTitleBar(int(size.x - (163 - button * 56) * scale),
                                         int(53 * scale), int(size.x), int(size.y)),
                       "Title bar overlaps a window button");
            expect(!mixer.hitTitleBar(int(700 * scale), int(150 * scale), int(size.x), int(size.y)),
                   "Title bar overlaps dashboard controls");
            bool found = false;
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (std::strstr(window->Name, "/Source list_")) {
                    found = true;
                    if (window->ScrollMax.y > 1.f) {
                        std::printf("Unexpected source scrolling: %.1f at %.0fx%.0f\n", window->ScrollMax.y,
                                    size.x, size.y);
                        ++failed;
                    }
                }
            }
            if (!found) { std::printf("Source list missing at %.0fx%.0f\n",size.x,size.y); ++failed; }
        }
        auto click = [&](float x, float y) {
            io.AddMousePosEvent(x, y);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, true);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, false);
            frame(1600, 986);
            frame(1600, 986);
        };
        frame(1600, 986);

        auto typePercent = [&](float x, float y, const char *value) {
            click(x, y);
            io.AddInputCharactersUTF8(value);
            frame(1600, 986);
            io.AddKeyEvent(ImGuiKey_Enter, true);
            frame(1600, 986);
            io.AddKeyEvent(ImGuiKey_Enter, false);
            frame(1600, 986);
            ImGui::ClearActiveID();
        };

        io.AddMousePosEvent(452, 153);
        frame(1600, 986);
        bool foundPaddedTooltip = false;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && (window->Flags & ImGuiWindowFlags_Tooltip)) {
                foundPaddedTooltip = true;
                expect(window->WindowPadding.x > 0 && window->WindowPadding.y > 0,
                       "Dashboard tooltip inherited zero root-window padding");
            }
        expect(foundPaddedTooltip, "Refresh tooltip did not appear");
        expect(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand,
               "Clickable dashboard control did not show a hand cursor on hover");
        bool foundHoverOutline = false;
        if (auto *dashboard = ImGui::FindWindowByName("Audio Monitor dashboard"))
            for (const ImDrawVert &vertex : dashboard->DrawList->VtxBuffer)
                foundHoverOutline |= vertex.col == ui::themePalette().hoverOutline &&
                                     vertex.pos.x >= 425 && vertex.pos.x <= 479 &&
                                     vertex.pos.y >= 125 && vertex.pos.y <= 180;
        expect(foundHoverOutline, "Clickable dashboard control had no visual hover outline");

        auto holdVolumeSlider = [&](float x, float y, float &volume, float expectedVolume) {
            io.AddMousePosEvent(x, y);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, true);
            frame(1600, 986);
            expect(std::abs(volume - expectedVolume) < .06f,
                   "Slider click set an unexpected volume");
            const float volumeOnPress = volume;
            for (int i = 0; i < 4; ++i) {
                frame(1600, 986);
                expect(std::abs(volume - volumeOnPress) < .001f,
                       "Holding a volume slider changed its value without moving the pointer");
            }
            io.AddMouseButtonEvent(0, false);
            frame(1600, 986);
            frame(1600, 986);
        };
        holdVolumeSlider(1030, 785, config.output.volume, .5f);
        holdVolumeSlider(260, 365, config.sources[0].volume, .5f);
        config.sources[0].gain = 2.f;
        click(435, 365); // Editable percentage at the right of the first device fader.
        io.AddInputCharactersUTF8("37");
        frame(1600, 986);
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame(1600, 986);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame(1600, 986);
        expect(std::abs(config.sources[0].volume - .37f) < .001f,
               "Typed device volume percentage was not applied");
        expect(config.sources[0].gain == 2.f,
               "Typing device volume changed its configured mix gain");
        config.output.gain = 2.f;
        typePercent(1485, 785, "43");
        expect(std::abs(config.output.volume - .43f) < .001f && config.output.gain == 2.f,
               "Typed primary output percentage did not update its independent volume");
        expect(std::abs(config.sources[0].volume - .37f) < .001f,
               "Editing output volume changed the source volume");
        ImGui::ClearActiveID();
        config.output.gain = 1.f;
        config.sources[0].gain = 1.f;
        config.output.volume = config.sources[0].volume = 1.f;
        const auto originalBuffer = config.bufferMillis;
        click(650, 488); // Sample Rate opens the Audio settings page.
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0,
               "Sample Rate did not open Settings");
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 448, dialog->DC.CursorStartPos.y + 327); // Buffer slider
            click(dialog->DC.CursorStartPos.x + 690, dialog->DC.CursorStartPos.y + 490); // Mono card edge
            click(dialog->DC.CursorStartPos.x + 633, dialog->DC.CursorStartPos.y + 716); // Save
        }
        expect(config.bufferMillis != originalBuffer,
               "Sample Rate did not open the Audio settings page");
        expect(config.mono, "Empty area of the Mono card was not clickable");
        config.bufferMillis = originalBuffer;
        engine.setBufferMillis(originalBuffer);
        config.mono = false;
        engine.setMono(false);
        click(900, 488); // Buffer also opens the Audio settings page.
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0,
               "Buffer did not open Settings");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 700, dialog->DC.CursorStartPos.y + 21); // Close
        expect(!popup(), "Buffer Settings did not close cleanly");
        click(1200, 488); // Channels
        expect(popup() && std::strcmp(popup()->Name, "Channels") == 0, "Channels dialog missing");
        expectFixedModal(popup(), "Channels is not modal");
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Channels blocks the uncovered main titlebar");
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 100, dialog->DC.CursorStartPos.y + 240); // Mono
            expect(config.mono && popup(), "Selecting Mono did not update the live mix");
            dialog = popup();
            click(dialog->DC.CursorStartPos.x + 520, dialog->DC.CursorStartPos.y + 21); // Close
        }
        expect(config.mono && !popup(), "Channels did not close cleanly");
        click(850, 785); // Master volume
        expect(config.output.volume < .9f, "Volume blocked after choosing channels");
        click(670, 900); // Settings
        expect(popup() != nullptr, "Settings blocked after adjusting volume");
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Settings blocks the uncovered main titlebar");
        if (auto *dialog = popup()) {
            const ImVec2 originalPosition = dialog->Pos;
            // A modal may overlap the titlebar after resize. Its controls must
            // retain input, but the uncovered app title remains draggable.
            ImGui::SetWindowPos(dialog, {400, 20});
            expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Titlebar steals a click inside Settings");
            expect(mixer.hitTitleBar(100, 50, 1600, 986), "Modal backdrop disables dragging from app title");
            expect(!mixer.hitTitleBar(1493, 53, 1600, 986), "Modal drag behavior overlaps maximize");
            ImGui::SetWindowPos(dialog, originalPosition);
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 80, dialog->DC.CursorStartPos.y + 349); // About
            expect(ui::MixerWindowTestAccess::settingsPage(mixer) == 4, "About did not open");
            const int beforeExport = changedFrames;
            std::promise<DiagnosticExportResult> exportResult;
            ui::MixerWindowTestAccess::setExportFuture(mixer, exportResult.get_future());
            click(dialog->DC.CursorStartPos.x + 448, dialog->DC.CursorStartPos.y + 457);
            expect(ui::MixerWindowTestAccess::exportPending(mixer), "Pending export was replaced by another click");
            exportResult.set_value({L"test-debug-log.txt", {}});
            frame(1600, 986);
            expect(!ui::MixerWindowTestAccess::exportPending(mixer) &&
                       ui::MixerWindowTestAccess::exportPath(mixer) == L"test-debug-log.txt" &&
                       !ui::MixerWindowTestAccess::exportFailed(mixer), "Completed export was not shown");
            std::promise<DiagnosticExportResult> failedExport;
            ui::MixerWindowTestAccess::setExportFuture(mixer, failedExport.get_future());
            failedExport.set_value({{}, "Test: access denied"});
            frame(1600, 986);
            expect(ui::MixerWindowTestAccess::exportFailed(mixer) &&
                       ui::MixerWindowTestAccess::exportPath(mixer) == L"test-debug-log.txt",
                   "Export failure was hidden or lost the previous log path");
            expect(changedFrames == beforeExport, "Debug log export dirtied audio preferences");
            // Exercise the real asynchronous report + config + file path in a
            // unique temporary directory, never the user's application data.
            wchar_t temporaryDirectory[MAX_PATH]{}, reservedPath[MAX_PATH]{};
            const DWORD temporaryLength = GetTempPathW(MAX_PATH, temporaryDirectory);
            const bool reserved = temporaryLength > 0 && temporaryLength < MAX_PATH &&
                GetTempFileNameW(temporaryDirectory, L"amd", 0, reservedPath) != 0;
            expect(reserved, "Could not reserve a temporary diagnostic export fixture");
            if (reserved && DeleteFileW(reservedPath)) {
                const std::wstring reportDirectory(reservedPath);
                ui::MixerWindowTestAccess::startExport(mixer, reportDirectory);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (ui::MixerWindowTestAccess::exportPending(mixer) &&
                       std::chrono::steady_clock::now() < deadline) {
                    frame(1600, 986);
                    Sleep(1);
                }
                const auto reportPath = ui::MixerWindowTestAccess::exportPath(mixer);
                const bool exported = !ui::MixerWindowTestAccess::exportPending(mixer) &&
                    !ui::MixerWindowTestAccess::exportFailed(mixer) &&
                    reportPath.starts_with(reportDirectory + L"\\");
                expect(exported, "Real asynchronous debug log export failed or did not finish");
                if (exported) {
                    std::ifstream report(std::filesystem::path(reportPath), std::ios::binary);
                    const std::string contents((std::istreambuf_iterator<char>(report)),
                                               std::istreambuf_iterator<char>());
                    expect(contents.find("Audio Monitor audio diagnostics") != std::string::npos &&
                               contents.find("Saved/live UI configuration") != std::string::npos &&
                               contents.find("Elgato 4K") != std::string::npos,
                           "Export omitted engine diagnostics or the current UI configuration");
                    report.close();
                    expect(DeleteFileW(reportPath.c_str()) != 0, "Could not clean up generated test log");
                }
                expect(RemoveDirectoryW(reportDirectory.c_str()) != 0, "Could not clean up test report directory");
                expect(changedFrames == beforeExport, "Real export changed audio preferences");
            }
            click(dialog->DC.CursorStartPos.x + 80,
                  dialog->DC.CursorStartPos.y + 291); // Keybinds navigation row.
            expect(ui::MixerWindowTestAccess::settingsPage(mixer) == 3,
                   "Keybinds settings category did not open");
            dialog = popup();
            click(dialog->DC.CursorStartPos.x + 80,
                  dialog->DC.CursorStartPos.y + 117); // Return to General.
            click(dialog->DC.CursorStartPos.x + 220, dialog->DC.CursorStartPos.y + 231); // Start hidden
            expect(!config.startMinimized, "Settings draft applied before Save");
            click(dialog->DC.CursorStartPos.x + 453, dialog->DC.CursorStartPos.y + 716); // Cancel
        }
        expect(!popup(), "Cancel did not close Settings");
        expect(!config.startMinimized, "Cancel did not discard Settings changes");
        click(670, 900); // Reopen Settings and verify Save commits the draft.
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 220, dialog->DC.CursorStartPos.y + 231); // Start hidden
            expect(!config.startMinimized, "Settings draft applied before Save");
            click(dialog->DC.CursorStartPos.x + 633, dialog->DC.CursorStartPos.y + 716); // Save
        }
        expect(!popup() && config.startMinimized, "Save did not commit Settings changes");
        config.startMinimized = false;
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Title bar stays blocked after closing Settings");
        click(1200, 488); // Channels again, with no Escape between any actions.
        expect(popup() && std::strcmp(popup()->Name, "Channels") == 0,
               "Channels blocked after closing Settings");
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 100, dialog->DC.CursorStartPos.y + 140); // Stereo
            expect(!config.mono && popup(), "Selecting Stereo did not update the live mix");
            dialog = popup();
            click(dialog->DC.CursorStartPos.x + 520, dialog->DC.CursorStartPos.y + 21); // Close
        }
        expect(!config.mono && !popup(), "Channels remained open after its close button");
        click(1440, 488); // Status
        expect(popup() && std::strcmp(popup()->Name, "Status") == 0, "Status dialog missing");
        expectFixedModal(popup(), "Status is not modal");
        click(700, 50); // Modal backdrop clicks must not dismiss Status.
        expect(popup() && std::strcmp(popup()->Name, "Status") == 0,
               "Backdrop click dismissed Status");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 550, dialog->DC.CursorStartPos.y + 21); // Close
        expect(!popup(), "Status close button did not dismiss the dialog");
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Title bar stays blocked after dismissing Status");
        click(670, 900);
        expect(popup() != nullptr, "Settings blocked after dismissing Status");
        auto expectPopupFits = [&](ImGuiWindow *dialog) {
            expect(dialog && dialog->Pos.x >= 0 && dialog->Pos.y >= 0 &&
                       dialog->Pos.x + dialog->Size.x <= io.DisplaySize.x &&
                       dialog->Pos.y + dialog->Size.y <= io.DisplaySize.y,
                   "Popup extends beyond the application");
        };
        auto dragAndResizePopup = [&](bool expectDrag = true) {
            auto *dialog = popup();
            if (!dialog) { expect(false, "Missing dialog for drag test"); return; }
            const ImVec2 original = dialog->Pos;
            const ImVec2 grab{dialog->Pos.x + dialog->Size.x / 2,
                              dialog->Pos.y + (dialog->TitleBarHeight > 0 ? dialog->TitleBarHeight / 2 : 20)};
            io.AddMousePosEvent(grab.x, grab.y);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, true);
            frame(1600, 986);
            io.AddMousePosEvent(grab.x + 20, grab.y + 20);
            frame(1600, 986);
            if (expectDrag)
                expect(dialog->Pos.x > original.x && dialog->Pos.y > original.y,
                       "Popup can no longer be dragged within the application");
            else
                expect(std::abs(dialog->Pos.x - original.x) < 1.f && std::abs(dialog->Pos.y - original.y) < 1.f,
                       "Fixed popup moved when dragged");
            for (auto target : {ImVec2(-500, 400), ImVec2(800, -500),
                                ImVec2(2100, 400), ImVec2(800, 1500)}) {
                io.AddMousePosEvent(target.x, target.y);
                frame(1600, 986);
                expectPopupFits(dialog);
            }
            io.AddMouseButtonEvent(0, false);
            frame(1600, 986);
            expectPopupFits(dialog);
            // Resize with the popup still open; check every frame, including
            // the transition while auto-sized content is being remeasured.
            for (int i = 0; i < 3; ++i) {
                frame(960, 600);
                expectPopupFits(dialog);
            }
            ImGui::SetScrollY(dialog, dialog->ScrollMax.y);
            frame(960, 600);
            expect(dialog->DC.CursorPosPrevLine.y >= dialog->Pos.y + dialog->TitleBarHeight &&
                       dialog->DC.CursorPosPrevLine.y + 20 <= dialog->Pos.y + dialog->Size.y,
                   "Popup action buttons are unreachable at minimum window size");
            for (auto size : {ImVec2(1440, 890), ImVec2(1600, 986)}) {
                for (int i = 0; i < 3; ++i) {
                    frame(int(size.x), int(size.y));
                    expectPopupFits(dialog);
                }
                expect(dialog->ScrollMax.y == 0, "Unnecessary scrollbar when the dialog fits");
            }
        };
        dragAndResizePopup(false); // Settings
        const Config routesBeforeRestore = config;
        const float volumeBeforeRestore = config.output.volume;
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 83, dialog->DC.CursorStartPos.y + 716); // Restore defaults
        expect(popup() && std::strcmp(popup()->Name, "Restore defaults?") == 0,
               "Confirmation popup missing");
        expect(mixer.hitTitleBar(100, 50, 1600, 986), "Nested confirmation disables main window dragging");
        if (auto &stack = ImGui::GetCurrentContext()->OpenPopupStack; stack.Size >= 2) {
            auto *parent = stack[stack.Size - 2].Window;
            const ImVec2 originalPosition = parent->Pos;
            ImGui::SetWindowPos(parent, {400, 20});
            expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Nested confirmation permits dragging through parent modal");
            ImGui::SetWindowPos(parent, originalPosition);
        }
        dragAndResizePopup(); // Nested confirmation.
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 20, dialog->DC.CursorPosPrevLine.y + 10); // Reset
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0,
               "Reset did not return to Settings");
        expect(config.output.volume < .9f, "Restore defaults applied before Save");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 633, dialog->DC.CursorStartPos.y + 716); // Save Settings
        expect(!popup() && config.sources.empty() && config.outputCount() == 0 && !engine.running(),
               "Saving Restore Defaults did not clear both Devices and Outputs and stop audio");
        // Restore only test fixtures for the independent editor coverage below.
        config.sources = routesBeforeRestore.sources;
        for (size_t output = 0; output < routesBeforeRestore.outputCount(); ++output)
            config.addOutput(routesBeforeRestore.outputAt(output));
        frame(1600, 986);

        const size_t sourceCountBeforeOutputEdit = config.sources.size();
        click(1514, 702); // Configure output device.
        expect(popup() && std::strcmp(popup()->Name, "Configure output") == 0,
               "Output editor did not open");
        expectFixedModal(popup(), "Output editor is not modal");
        dragAndResizePopup(false);
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 151, dialog->DC.CursorStartPos.y + 346); // Chat icon
            click(dialog->DC.CursorStartPos.x + 400, dialog->DC.CursorStartPos.y + 340); // Name
            io.AddInputCharactersUTF8("Stream Output");
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 308, dialog->DC.CursorStartPos.y + 495); // 200% gain
            expect(config.output.icon.empty() && config.output.label.empty() && config.output.gain == 1.f,
                   "Output editor applied its draft before Save");
            click(dialog->DC.CursorStartPos.x + 458, dialog->DC.CursorStartPos.y + 634); // Cancel
        }
        expect(!popup() && config.output.icon.empty() && config.output.label.empty() &&
                   config.output.gain == 1.f,
               "Cancel did not discard output changes");

        click(1514, 702); // Configure output again and commit.
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 151, dialog->DC.CursorStartPos.y + 346); // Chat icon
            click(dialog->DC.CursorStartPos.x + 400, dialog->DC.CursorStartPos.y + 340); // Name
            io.AddInputCharactersUTF8("Stream Output");
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 308, dialog->DC.CursorStartPos.y + 495); // 200% gain
            click(dialog->DC.CursorStartPos.x + 634, dialog->DC.CursorStartPos.y + 492); // Gain field
            io.AddInputCharactersUTF8("400");
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 634, dialog->DC.CursorStartPos.y + 634); // Save
        }
        expect(!popup() && config.output.icon == "chat" && config.output.label == "Stream Output" &&
                   config.output.gain == 4.f && config.output.volume == volumeBeforeRestore,
               "Typed output gain was not committed");
        expect(config.sources.size() == sourceCountBeforeOutputEdit,
               "Editing output accidentally changed the source list");
        holdVolumeSlider(1030, 785, config.output.volume, .5f);
        expect(config.output.gain == 4.f,
               "Master volume changed the configured output mix gain");

        const std::vector<DeviceInfo> outputFixtures{
            // Saved defaults use a friendly-name substring; enumeration adds
            // the endpoint ID and returns the device's full friendly name.
            {L"primary-endpoint", L"Elgato 4K Pro Capture Device", true, true},
            {L"secondary-endpoint", L"Studio Speakers", true, false},
            {L"third-endpoint", L"Monitor Speakers", true, false},
        };
        auto injectOutputFixtures = [&] {
            // Opening an output dialog refreshes real endpoints first. Inject
            // only after it is open so the picker remains deterministic here.
            ui::MixerWindowTestAccess::setPlaybackDevices(mixer, outputFixtures);
            frame(1600, 986);
        };
        auto chooseOutputFixture = [&](size_t fixtureIndex) {
            auto *dialog = popup();
            expect(dialog && std::strcmp(dialog->Name, "Configure output") == 0,
                   "Output picker had no parent dialog");
            if (!dialog)
                return;
            click(dialog->DC.CursorStartPos.x + 250,
                  dialog->DC.CursorStartPos.y + 152);
            auto *picker = popup();
            expect(picker && picker != dialog, "Output device combo did not open");
            if (!picker || picker == dialog)
                return;
            // The dashboard's scaled font has already been popped here.
            // Measure the actual combo rows instead of using the harness's
            // smaller default font (which clicks the wrong third row).
            const float spacing = ImGui::GetStyle().ItemSpacing.y;
            const float rowStep = (picker->DC.CursorMaxPos.y - picker->DC.CursorStartPos.y + spacing) /
                                  static_cast<float>(outputFixtures.size());
            click(picker->DC.CursorStartPos.x + 30,
                  picker->DC.CursorStartPos.y + (rowStep - spacing) * .5f +
                      rowStep * float(fixtureIndex));
        };
        auto pressEscape = [&] {
            io.AddKeyEvent(ImGuiKey_Escape, true);
            frame(1600, 986);
            io.AddKeyEvent(ImGuiKey_Escape, false);
            frame(1600, 986);
        };
        auto sameChannel = [](const ChannelConfig &a, const ChannelConfig &b) {
            return a.label == b.label && a.kind == b.kind && a.enabled == b.enabled &&
                   a.processPath == b.processPath && a.deviceId == b.deviceId &&
                   a.deviceNameMatch == b.deviceNameMatch && a.icon == b.icon &&
                   a.gain == b.gain && a.volume == b.volume && a.muted == b.muted &&
                   a.muteKeybind == b.muteKeybind;
        };

        click(1480, 618); // Add Output.
        expect(popup() && std::strcmp(popup()->Name, "Configure output") == 0,
               "Add Output did not open the output dialog");
        injectOutputFixtures();
        chooseOutputFixture(1); // A second, distinct playback endpoint.
        expect(ui::MixerWindowTestAccess::outputDraft(mixer).deviceId == L"secondary-endpoint",
               "Output picker did not select the distinct second endpoint");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 634); // Add Output.
        expect(!popup() && config.outputCount() == 2 &&
                   config.additionalOutputs[0].deviceId == L"secondary-endpoint",
               "Adding a second distinct output did not commit it");
        expect(ui::MixerWindowTestAccess::selectedOutput(mixer) == 1,
               "Newly added output was not selected on the dashboard");

        click(1282, 618); // Previous output.
        expect(ui::MixerWindowTestAccess::selectedOutput(mixer) == 0,
               "Previous Output did not select the primary output");
        click(1387, 618); // Next output.
        expect(ui::MixerWindowTestAccess::selectedOutput(mixer) == 1,
               "Next Output did not return to the second output");

        const ChannelConfig primaryBeforePercentEdit = config.output;
        const float sourceVolumeBeforeOutputEdit = config.sources[0].volume;
        const float secondaryMixGain = config.additionalOutputs[0].gain;
        typePercent(1485, 785, "135");
        expect(config.additionalOutputs[0].volume == 1.f,
               "Output percentage accepted a volume above 100%");
        typePercent(1485, 785, "-12");
        expect(config.additionalOutputs[0].volume == 0.f,
               "Output percentage accepted a negative volume");
        typePercent(1485, 785, "64");
        expect(std::abs(config.additionalOutputs[0].volume - .64f) < .001f &&
                   config.additionalOutputs[0].gain == secondaryMixGain,
               "Typed secondary output percentage did not preserve its mix gain");
        expect(sameChannel(config.output, primaryBeforePercentEdit) &&
                   config.sources[0].volume == sourceVolumeBeforeOutputEdit,
               "Editing the second output percentage changed another channel");

        // Leave its percentage focused, then navigate between outputs. The
        // input buffer must not transfer a value to a different destination.
        click(1485, 785);
        io.AddInputCharactersUTF8("57");
        frame(1600, 986);
        click(1282, 618); // Previous output while its sibling field is active.
        expect(sameChannel(config.output, primaryBeforePercentEdit),
               "Output navigation applied the previous percentage edit to the next device");
        click(1387, 618);
        expect(std::abs(config.additionalOutputs[0].volume - .57f) < .001f,
               "Output navigation lost the selected output's percentage edit");

        const ChannelConfig primaryBeforeSecondaryEdit = config.output;
        click(1514, 702); // Configure the selected second output.
        injectOutputFixtures();
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 400,
                  dialog->DC.CursorStartPos.y + 340); // Display Name.
            io.AddInputCharactersUTF8("Secondary Output");
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 634); // Save.
        }
        expect(!popup() && config.outputCount() == 2 &&
                   config.additionalOutputs[0].label == "Secondary Output",
               "Editing the second output did not commit to that output");
        expect(sameChannel(config.output, primaryBeforeSecondaryEdit),
               "Editing the second output mutated the primary output");

        // Simulate a saved endpoint ID that became stale after a driver
        // reinstall. Its friendly-name fallback still resolves to fixture 0,
        // which must not be addable as a second copy of the same endpoint.
        config.output.deviceId = L"stale-primary-endpoint";
        click(1480, 618); // Begin adding a third output.
        injectOutputFixtures();
        chooseOutputFixture(0); // The primary endpoint is already in use.
        const bool duplicatePickerDisabled =
            ui::MixerWindowTestAccess::outputDraft(mixer).deviceId.empty() &&
            ui::MixerWindowTestAccess::outputDraft(mixer).deviceNameMatch.empty() &&
            config.outputCount() == 2 && popup() &&
            std::strcmp(popup()->Name, "Configure output") != 0;
        expect(duplicatePickerDisabled,
               "Duplicate output endpoint was not disabled in the picker");
        bool foundDuplicateTooltip = false;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            foundDuplicateTooltip |= window->Active &&
                                     (window->Flags & ImGuiWindowFlags_Tooltip);
        expect(!duplicatePickerDisabled || foundDuplicateTooltip,
               "Disabled duplicate output had no explanatory tooltip");
        if (duplicatePickerDisabled)
            pressEscape(); // Close only the combo popup.
        ui::MixerWindowTestAccess::setOutputDraftDevice(
            mixer, L"primary-endpoint", L"Elgato 4K Pro Capture Device");
        frame(1600, 986);
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 634); // Disabled Add Output.
        const bool duplicateSaveRejected =
            popup() && std::strcmp(popup()->Name, "Configure output") == 0 &&
            config.outputCount() == 2;
        expect(duplicateSaveRejected,
               "Duplicate output endpoint bypassed dialog validation");
        if (duplicateSaveRejected) {
            auto *dialog = popup();
            click(dialog->DC.CursorStartPos.x + 458,
                  dialog->DC.CursorStartPos.y + 634); // Cancel.
        } else {
            // Keep the remaining checks independent if duplicate validation
            // regresses and the invalid third destination was committed.
            config.additionalOutputs.resize(1);
            ui::MixerWindowTestAccess::setSelectedOutput(mixer, 1);
            frame(1600, 986);
        }
        config.output.deviceId = primaryBeforeSecondaryEdit.deviceId;

        click(1514, 702); // Configure the still-selected second output.
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 82,
                  dialog->DC.CursorStartPos.y + 634); // Remove.
        expect(!popup() && config.outputCount() == 1 &&
                   sameChannel(config.output, primaryBeforeSecondaryEdit),
               "Removing the second output did not leave the primary intact");
        expect(ui::MixerWindowTestAccess::selectedOutput(mixer) == 0,
               "Removing the selected output left an invalid selection");

        for (int i = 0; i < kMaxOutputs - 1; ++i) {
            ChannelConfig extra;
            extra.deviceId = L"limit-endpoint-" + std::to_wstring(i);
            extra.deviceNameMatch = L"Limit Output " + std::to_wstring(i);
            config.additionalOutputs.push_back(std::move(extra));
        }
        frame(1600, 986);
        click(1480, 618); // Output Limit at four configured destinations.
        expect(config.outputCount() == kMaxOutputs &&
                   config.additionalOutputs.size() == size_t(kMaxOutputs - 1),
               "Output-limit interaction changed the configured output count");
        auto *dashboard = ImGui::FindWindowByName("Audio Monitor dashboard");
        const auto &popupStack = ImGui::GetCurrentContext()->OpenPopupStack;
        expect(dashboard && !popupStack.empty() &&
                   popupStack.back().PopupId == dashboard->GetID("Output limit"),
               "Output-limit interaction did not open its limit notice");
        expect(!mixer.hitTitleBar(100, 50, 1600, 986), "Titlebar steals outside-click dismissal from a menu");
        pressEscape();
        config.additionalOutputs.clear();
        frame(1600, 986);

        config.sources[0].volume = 1.f;
        click(440, 239); // Configure the first source.
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0, "Source popup missing");
        expect(mixer.hitTitleBar(100, 50, 1600, 986), "Source dialog blocks dragging from the app title");
        expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Main titlebar steals clicks from tall source dialog");
        dragAndResizePopup(false);

        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 708); // Source gain field
            io.AddInputCharactersUTF8("400");
            frame(1600, 986);
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 830); // Save source
        }
        expect(!popup() && config.sources[0].gain == 4.f && config.sources[0].volume == 1.f,
               "Saving a 400% source mix gain changed its card volume");
        holdVolumeSlider(260, 365, config.sources[0].volume, .5f);
        expect(config.sources[0].gain == 4.f,
               "Source card volume changed the configured source mix gain");

        click(440, 239); // Reopen the first source for dialog behavior tests.
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0, "Source popup missing");
        dragAndResizePopup(false);

        if (auto *dialog = popup()) {
            const ImVec2 origin = dialog->DC.CursorStartPos;
            bool foundGainHelper = false;
            float helperMaxY = origin.y;
            for (const ImDrawVert &vertex : dialog->DrawList->VtxBuffer) {
                if (vertex.col != IM_COL32(173, 181, 190, 255) ||
                    vertex.pos.x < origin.x + 15 || vertex.pos.x > origin.x + 707 ||
                    vertex.pos.y < origin.y + 758 || vertex.pos.y > origin.y + 800)
                    continue;
                foundGainHelper = true;
                helperMaxY = std::max(helperMaxY, vertex.pos.y);
            }
            expect(foundGainHelper && helperMaxY < origin.y + 784,
                   "Source gain helper text overlaps its panel outline");
        }

        const size_t sourceCountBeforeDialogTests = config.sources.size();
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 520,
                  dialog->DC.CursorStartPos.y + 200); // Application Audio card
            dialog = popup();
            const float helperTop = dialog->DC.CursorStartPos.y + 395;
            const float helperBottom = dialog->DC.CursorStartPos.y + 425;
            const float helperLeft = dialog->DC.CursorStartPos.x + 15;
            const float helperRight = dialog->DC.CursorStartPos.x + 707;
            bool grayHelperText = false, amberHelperText = false;
            for (const ImDrawVert &vertex : dialog->DrawList->VtxBuffer) {
                if (vertex.pos.x < helperLeft || vertex.pos.x > helperRight ||
                    vertex.pos.y < helperTop || vertex.pos.y > helperBottom)
                    continue;
                grayHelperText |= vertex.col == IM_COL32(173, 181, 190, 255);
                amberHelperText |= vertex.col == IM_COL32(255, 174, 51, 255);
            }
            expect(grayHelperText != amberHelperText,
                   "Source dialog drew two helper sentences at the same origin");
            click(dialog->DC.CursorStartPos.x + 458,
                  dialog->DC.CursorStartPos.y + 830); // Cancel edit
        }
        expect(!popup() && config.sources.size() == sourceCountBeforeDialogTests,
               "Cancel source edit changed the source list");

        click(260, 909); // Add Device.
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0,
               "Add Device did not open the source dialog");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 604,
                  dialog->DC.CursorStartPos.y + 830); // Disabled Add Source
        expect(popup() && config.sources.size() == sourceCountBeforeDialogTests,
               "Invalid Add Source committed an empty source");
        ui::MixerWindowTestAccess::makeSourceDraftValid(mixer);
        frame(1600, 986);
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 604,
                  dialog->DC.CursorStartPos.y + 830); // Valid Add Source
        expect(!popup() && config.sources.size() == sourceCountBeforeDialogTests + 1,
               "Valid Add Source did not commit the source");

        click(260, 909); // Add Device, then cancel a valid draft.
        ui::MixerWindowTestAccess::makeSourceDraftValid(mixer);
        frame(1600, 986);
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 396,
                  dialog->DC.CursorStartPos.y + 830); // Cancel add
        expect(!popup() && config.sources.size() == sourceCountBeforeDialogTests + 1,
               "Cancel Add Source changed the source list");

        click(440, 239); // Remove the first source.
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 82,
                  dialog->DC.CursorStartPos.y + 830); // Remove
        expect(!popup() && config.sources.size() == sourceCountBeforeDialogTests,
               "Remove source did not remove exactly one source");

        int keyboardWidth = 1600, keyboardHeight = 986;
        auto pressKey = [&](ImGuiKey key) {
            io.AddKeyEvent(key, true);
            frame(keyboardWidth, keyboardHeight);
            io.AddKeyEvent(key, false);
            frame(keyboardWidth, keyboardHeight);
        };
        auto focusDashboardItem = [&](const char *label) {
            frame(keyboardWidth, keyboardHeight);
            auto *dashboard = ImGui::FindWindowByName("Audio Monitor dashboard");
            expect(dashboard != nullptr, "Dashboard missing for keyboard test");
            if (!dashboard)
                return;
            ImGui::SetNavWindow(dashboard);
            const ImGuiID itemId = std::strcmp(label, "##master-volume") == 0
                                      ? ImHashStr(label, 0, dashboard->GetID(
                                            static_cast<int>(ui::MixerWindowTestAccess::selectedOutput(mixer))))
                                      : dashboard->GetID(label);
            ImGui::SetNavID(itemId, ImGuiNavLayer_Main,
                            dashboard->NavRootFocusScopeId, ImRect());
            ImGui::GetCurrentContext()->NavCursorVisible = true;
            frame(keyboardWidth, keyboardHeight);
        };

        auto sourceList = [&]() -> ImGuiWindow * {
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (std::strstr(window->Name, "/Source list_"))
                    return window;
            return nullptr;
        };
        auto sourceItemId = [&](int index, const char *label) {
            auto *list = sourceList();
            return list ? ImHashStr(label, 0, list->GetID(index)) : ImGuiID(0);
        };
        auto expectFocus = [&](ImGuiID id, const char *message) {
            auto *context = ImGui::GetCurrentContext();
            if (id != context->NavId && context->NavWindow) {
                const auto &rect = context->NavWindow->NavRectRel[context->NavLayer];
                std::printf("Focus expected %08X got %08X in %s at %.0f,%.0f-%.0f,%.0f\n",
                            id, context->NavId, context->NavWindow->Name,
                            rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y);
            }
            expect(id != 0 && context->NavId == id && context->NavCursorVisible, message);
        };
        auto hasFocusOutline = [&]() {
            auto *context = ImGui::GetCurrentContext();
            auto *window = context->NavWindow;
            if (!window) return false;
            ImRect bounds = window->NavRectRel[context->NavLayer];
            bounds.Translate(window->Pos);
            bounds.Expand(5.f);
            int accentVertices = 0;
            for (const auto &vertex : window->DrawList->VtxBuffer)
                if (vertex.col == ui::themePalette().accent && bounds.Contains(vertex.pos))
                    ++accentVertices;
            return accentVertices >= 8;
        };

        // Cross the scrolling child border in one key press, landing on an
        // actual control, not the list container. Exercise real nav scoring
        // at multiple dashboard scales, including the user's screenshot size.
        for (const auto size : {ImVec2(1600, 986), ImVec2(1440, 890), ImVec2(960, 600)}) {
            keyboardWidth = static_cast<int>(size.x);
            keyboardHeight = static_cast<int>(size.y);
            for (const auto theme : {ColorTheme::Dark, ColorTheme::Light}) {
                ui::applyTheme(theme);
                focusDashboardItem("Refresh devices");
                pressKey(ImGuiKey_DownArrow);
                expectFocus(sourceItemId(0, "Configure source"),
                            "Down from Refresh did not visibly focus the nearest source button");
                expect(hasFocusOutline(), "Focused source button has no visible outline");
                pressKey(ImGuiKey_LeftArrow);
                expectFocus(sourceItemId(0, "Enable source"),
                            "Left did not focus the adjacent source toggle");
                pressKey(ImGuiKey_RightArrow);
                expectFocus(sourceItemId(0, "Configure source"),
                            "Right did not return to the adjacent source button");
                pressKey(ImGuiKey_UpArrow);
                expectFocus(ImGui::FindWindowByName("Audio Monitor dashboard")->GetID("Refresh devices"),
                            "Up did not navigate out of the source list to Refresh");
                pressKey(ImGuiKey_DownArrow);
                pressKey(ImGuiKey_DownArrow);
                expectFocus(sourceItemId(0, "##source-volume-percent"),
                            "Down from Configure did not focus the nearest volume percentage field");
                expect(hasFocusOutline(), "Focused native percentage field has no visible outline");
            }
        }
        keyboardWidth = 1600;
        keyboardHeight = 986;
        ui::applyTheme(config.colorTheme);
        focusDashboardItem("Refresh devices");
        pressKey(ImGuiKey_DownArrow);
        pressKey(ImGuiKey_RightArrow);
        auto *keyboardDashboard = ImGui::FindWindowByName("Audio Monitor dashboard");
        expectFocus(ImHashStr("Audio information", 0, keyboardDashboard->GetID(600)),
                    "Right from a source did not reach the nearest dashboard information button");
        pressKey(ImGuiKey_LeftArrow);
        expectFocus(sourceItemId(1, "Configure source"),
                    "Left from dashboard information did not reach the nearest source control");

        focusDashboardItem("Refresh devices");
        pressKey(ImGuiKey_DownArrow);
        pressKey(ImGuiKey_DownArrow);
        const float savedKeyboardVolume = config.sources.front().volume;
        pressKey(ImGuiKey_Enter);
        io.AddInputCharactersUTF8("73");
        frame(1600, 986);
        pressKey(ImGuiKey_LeftArrow);
        expect(ImGui::GetCurrentContext()->ActiveId == sourceItemId(0, "##source-volume-percent"),
               "Arrow navigation stole the caret from an active volume field");
        pressKey(ImGuiKey_Enter);
        expect(std::abs(config.sources.front().volume - .73f) < .001f,
               "Keyboard-focused volume percentage could not be edited");
        config.sources.front().volume = savedKeyboardVolume;

        focusDashboardItem("Refresh devices");
        pressKey(ImGuiKey_DownArrow);
        pressKey(ImGuiKey_Enter);
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0,
               "Enter after Refresh/Down did not open the focused source");
        for (auto key : {ImGuiKey_DownArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_LeftArrow}) {
            pressKey(key);
            expect(popup() && ImGui::GetCurrentContext()->NavWindow &&
                       ImGui::GetCurrentContext()->NavWindow->RootWindowForNav == popup(),
                   "Dashboard arrow navigation escaped an open modal");
        }
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 700,
                  dialog->DC.CursorStartPos.y + 21);
        expect(!popup(), "Keyboard-opened source configuration did not close");

        focusDashboardItem("Refresh devices");
        const ImGuiID firstTabId = ImGui::GetCurrentContext()->NavId;
        pressKey(ImGuiKey_Tab);
        const ImGuiID secondTabId = ImGui::GetCurrentContext()->NavId;
        pressKey(ImGuiKey_Tab);
        expect(firstTabId != 0 && secondTabId != 0 && firstTabId != secondTabId &&
                   ImGui::GetCurrentContext()->NavId != secondTabId,
               "Tab did not advance through dashboard controls in order");

        focusDashboardItem("Add Device");
        pressKey(ImGuiKey_Space);
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0,
               "Space did not activate a custom hit target");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 700,
                  dialog->DC.CursorStartPos.y + 21); // Close source dialog
        expect(!popup(), "Keyboard-opened source dialog did not close");

        focusDashboardItem("Settings");
        pressKey(ImGuiKey_Enter);
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0,
               "Enter did not open Settings");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 700,
                  dialog->DC.CursorStartPos.y + 21); // Close Settings
        expect(!popup(), "Keyboard-opened Settings did not close");

        config.output.gain = 1.f;
        config.output.volume = .5f;
        frame(1600, 986);
        focusDashboardItem("##master-volume");
        pressKey(ImGuiKey_Space);
        const float normalVolumeBefore = config.output.volume;
        pressKey(ImGuiKey_RightArrow);
        const float normalVolumeStep = config.output.volume - normalVolumeBefore;
        expect(normalVolumeStep > 0.f && normalVolumeStep < .02f,
               "Keyboard slider did not use its 0-100% volume range");
        ImGui::ClearActiveID();

        config.output.gain = 4.f;
        config.output.volume = .5f;
        frame(1600, 986);
        focusDashboardItem("##master-volume");
        pressKey(ImGuiKey_Space);
        const float boostedVolumeBefore = config.output.volume;
        pressKey(ImGuiKey_RightArrow);
        const float boostedVolumeStep = config.output.volume - boostedVolumeBefore;
        expect(boostedVolumeStep > 0.f &&
                   std::abs(boostedVolumeStep - normalVolumeStep) < .002f &&
                   config.output.gain == 4.f,
               "Mix gain changed the keyboard volume range or value");
        ImGui::ClearActiveID();

        const auto savedSources = config.sources;
        while (config.sources.size() < kMaxSources)
            config.sources.push_back(config.sources.front());
        frame(1600, 986);
        frame(1600, 986);
        focusDashboardItem("Refresh devices");
        pressKey(ImGuiKey_DownArrow);
        for (int index = 0; index < kMaxSources; ++index) {
            expectFocus(sourceItemId(index, "Configure source"),
                        "Arrow navigation skipped a source card in the scrolling list");
            pressKey(ImGuiKey_DownArrow);
            expectFocus(sourceItemId(index, "##source-volume-percent"),
                        "Arrow navigation stopped on a non-control in the scrolling list");
            pressKey(ImGuiKey_DownArrow);
        }
        expect(sourceList() && sourceList()->Scroll.y > 0.f,
               "Arrow navigation did not scroll clipped source cards into view");
        expectFocus(ImGui::FindWindowByName("Audio Monitor dashboard")->GetID("Add Device"),
                    "Down from the final source did not leave the list for Add Device");
        pressKey(ImGuiKey_UpArrow);
        expectFocus(sourceItemId(kMaxSources - 1, "##source-volume"),
                    "Up from Add Device did not reach the nearest volume slider");
        pressKey(ImGuiKey_RightArrow);
        for (int index = kMaxSources - 1; index >= 0; --index) {
            expectFocus(sourceItemId(index, "##source-volume-percent"),
                        "Up did not return through the nearest volume percentage fields");
            pressKey(ImGuiKey_UpArrow);
            expectFocus(sourceItemId(index, "Configure source"),
                        "Up did not reach the nearest source configuration button");
            pressKey(ImGuiKey_UpArrow);
        }
        expectFocus(ImGui::FindWindowByName("Audio Monitor dashboard")->GetID("Refresh devices"),
                    "Up from the first source did not return to Refresh after scrolling");
        click(260, 909);
        expect(popup() != nullptr, "Source limit popup did not open at the source cap");
        pressKey(ImGuiKey_Escape);
        expect(!popup(), "Escape did not close the Source limit popup");
        config.sources = savedSources;

        const ImU32 darkBackground = ui::themePalette().background;
        click(670, 900); // Settings, General.
        const int changedBeforeTheme = changedFrames;
        const bool closeToTrayBeforeTheme = config.closeToTray;
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 230,
                  dialog->DC.CursorStartPos.y + 290); // Stage another preference.
            click(dialog->DC.CursorStartPos.x + 316,
                  dialog->DC.CursorStartPos.y + 633); // Light theme.
            expect(config.colorTheme == ColorTheme::Light &&
                       ui::themePalette().background != darkBackground &&
                       changedFrames > changedBeforeTheme,
                   "Theme selection did not immediately apply and request persistence");
            const auto &palette = ui::themePalette();
            expect(palette.sliderThumb != palette.onAccent && palette.sliderThumb != palette.card &&
                       palette.sliderThumb != palette.sliderTrack && palette.sliderThumbBorder != palette.sliderThumb,
                   "Light slider thumb has no distinct fill and outline");
            bool foundThumb = false;
            for (const auto *list : ImGui::GetDrawData()->CmdLists)
                for (const auto &vertex : list->VtxBuffer)
                    foundThumb |= vertex.col == palette.sliderThumbBorder;
            expect(foundThumb, "Dashboard sliders did not draw the theme's visible thumb outline");
            dialog = popup();
            click(dialog->DC.CursorStartPos.x + 453,
                  dialog->DC.CursorStartPos.y + 716); // Cancel remaining settings.
        }
        frame(1600, 986);
        expect(config.colorTheme == ColorTheme::Light &&
                   ui::themePalette().background != darkBackground &&
                   config.closeToTray == closeToTrayBeforeTheme,
               "Cancel reverted the already-committed light theme");

        click(670, 900); // Reopen General settings.
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 426,
                  dialog->DC.CursorStartPos.y + 633); // System theme.
            expect(config.colorTheme == ColorTheme::System,
                   "System theme did not become live before saving");
            dialog = popup();
            click(dialog->DC.CursorStartPos.x + 700,
                  dialog->DC.CursorStartPos.y + 21); // Close without saving.
        }
        expect(config.colorTheme == ColorTheme::System,
               "Closing settings reverted the System theme choice");

        auto restorePreferences = [&](bool save = true) {
            click(670, 900);
            auto *settings = popup();
            expect(settings && std::strcmp(settings->Name, "Settings") == 0,
                   "Could not open settings for restore");
            if (!settings) return;
            click(settings->DC.CursorStartPos.x + 83,
                  settings->DC.CursorStartPos.y + 716);
            auto *confirm = popup();
            expect(confirm && std::strcmp(confirm->Name, "Restore defaults?") == 0,
                   "Restore did not ask for confirmation");
            if (!confirm || confirm == settings) return;
            click(confirm->DC.CursorStartPos.x + 25,
                  confirm->DC.CursorMaxPos.y - ImGui::GetFrameHeight() * .5f);
            settings = popup();
            expect(settings && std::strcmp(settings->Name, "Settings") == 0,
                   "Reset did not return to the settings draft");
            if (settings)
                click(settings->DC.CursorStartPos.x + (save ? 633 : 453),
                      settings->DC.CursorStartPos.y + 716);
            expect(!popup(), "Restored settings could not be saved or canceled");
        };
        config.monitoringKeybind = {MOD_CONTROL | MOD_ALT, 'R'};
        config.sources.front().muteKeybind = {MOD_CONTROL | MOD_ALT, 'S'};
        config.output.muteKeybind = {MOD_CONTROL | MOD_ALT, 'O'};
        const auto monitoringKeybindBeforeRestore = config.monitoringKeybind;
        const auto sourceRoutesBeforeRestore = config.sources;
        const auto outputBeforeRestore = config.output;
        config.bufferMillis = 150;
        AudioEngineTestAccess::prepareMeterSession(engine, config);
        AudioEngineTestAccess::setMonitoringState(engine, true);
        restorePreferences(false);
        expect(config.outputCount() == 1 && sameChannel(config.output, outputBeforeRestore) &&
                   config.sources.size() == sourceRoutesBeforeRestore.size() &&
                   std::equal(config.sources.begin(), config.sources.end(),
                              sourceRoutesBeforeRestore.begin(), sameChannel) &&
                   config.bufferMillis == 150 && engine.monitoring() &&
                   config.monitoringKeybind == monitoringKeybindBeforeRestore,
               "Cancel committed a staged Restore Defaults");
        restorePreferences();
        expect(config.sources.empty() && config.outputCount() == 0 && !engine.running() &&
                   config.monitoringKeybind == Keybind{} &&
                   config.bufferMillis == Config::defaults().bufferMillis,
               "Confirmed and saved Restore Defaults did not leave an empty setup");
        // Subsequent ordinary Save must not retain the reset request.
        config.sources = sourceRoutesBeforeRestore;
        config.addOutput(outputBeforeRestore);
        click(670, 900);
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 633, dialog->DC.CursorStartPos.y + 716);
        expect(config.outputCount() == 1 && config.sources.size() == sourceRoutesBeforeRestore.size(),
               "A normal Save repeated an old device reset");

        click(1514, 702); // Configure the last remaining output.
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 82,
                  dialog->DC.CursorStartPos.y + 634); // Remove last output.
        expect(!popup() && config.outputCount() == 0 && config.additionalOutputs.empty() &&
                   ui::MixerWindowTestAccess::selectedOutput(mixer) == 0,
               "Last output could not be removed without an invalid selection");
        frame(1600, 986);
        click(1000, 900); // Start Monitoring is unavailable with no destination.
        expect(!engine.monitoring() &&
                   ui::MixerWindowTestAccess::statusDetail(mixer).find("Add an output") != std::string::npos,
               "Empty outputs started forwarding or reported a healthy live route");
        focusDashboardItem("Settings");
        pressKey(ImGuiKey_RightArrow);
        expect(ImGui::GetCurrentContext()->NavId !=
                   ImGui::FindWindowByName("Audio Monitor dashboard")->GetID("Toggle monitoring"),
               "Arrow navigation focused the unavailable Start Monitoring button");
        restorePreferences();
        expect(config.sources.empty() && config.outputCount() == 0,
               "Restore defaults failed to clear sources or recreated a removed output");

        // Even with no outputs the visible source meters remain useful.
        config.sources = sourceRoutesBeforeRestore;
        AudioEngineTestAccess::prepareMeterSession(engine, config);
        mixer.setVisible(true);
        engine.channelPeak(0).l.publish(.8f);
        frame(1600, 986);
        expect(ui::MixerWindowTestAccess::sourceMeterLevelDb(mixer, 0) > -3.f &&
                   !engine.monitoring(), "Empty outputs prevented visible source metering");
        mixer.setVisible(false);

        click(1480, 618); // Add the first output back into the empty list.
        injectOutputFixtures();
        chooseOutputFixture(2);
        expect(ui::MixerWindowTestAccess::outputDraft(mixer).deviceId == L"third-endpoint",
               "Empty-list picker did not select the requested endpoint");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 634,
                  dialog->DC.CursorStartPos.y + 634);
        expect(!popup() && config.outputCount() == 1 && config.additionalOutputs.empty() &&
                   config.output.deviceId == L"third-endpoint" &&
                   ui::MixerWindowTestAccess::selectedOutput(mixer) == 0,
               "Adding an output to an empty list did not create the primary destination");

        click(670, 900); // Runtime can resolve a replacement ID while Settings is open.
        config.output.deviceId = L"freshly-resolved-output-id";
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 633,
                  dialog->DC.CursorStartPos.y + 716);
        expect(!popup() && config.output.deviceId == L"freshly-resolved-output-id",
               "Saving preferences restored an obsolete output ID from the settings draft");

        // Capture shortcuts through the real modal and keep them staged until
        // Settings is saved. No global keys or audio devices are opened.
        config.monitoringKeybind = {};
        for (auto &source : config.sources) source.muteKeybind = {};
        for (size_t i = 0; i < config.outputCount(); ++i) config.outputAt(i).muteKeybind = {};
        auto openKeybindSettings = [&] {
            click(670, 900);
            if (auto *settings = popup())
                click(settings->DC.CursorStartPos.x + 80, settings->DC.CursorStartPos.y + 291);
            expect(popup() && std::strcmp(popup()->Name, "Settings") == 0 &&
                       ui::MixerWindowTestAccess::settingsPage(mixer) == 3,
                   "Could not open Keybinds settings");
        };
        auto keybindList = [&]() -> ImGuiWindow * {
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && std::strstr(window->Name, "/Keybind list_")) return window;
            return nullptr;
        };
        auto clickKeybindRow = [&](int target, bool clear = false) {
            auto *list = keybindList();
            expect(list != nullptr, "Keybind list is missing");
            if (!list) return;
            const int ordinal = target <= kMaxSources ? target :
                static_cast<int>(ui::MixerWindowTestAccess::settingsDraft(mixer).sources.size()) +
                    target - kMaxSources;
            const float rowStep = 96.f + ImGui::GetStyle().ItemSpacing.y;
            ImGui::SetScrollY(list, std::max(0.f, ordinal * rowStep - 100.f));
            frame(1600, 986);
            click(list->DC.CursorStartPos.x + (clear ? list->WorkRect.GetWidth() - 38.f : 30.f),
                  list->DC.CursorStartPos.y + ordinal * rowStep + 56.f);
            if (!clear)
                expect(popup() && std::strcmp(popup()->Name, "Set keybind") == 0 &&
                           ui::MixerWindowTestAccess::keybindTarget(mixer) == target,
                       "A shortcut field opened the wrong capture target");
        };
        auto closeCapture = [&](bool accept) {
            auto *capture = popup();
            expect(capture && std::strcmp(capture->Name, "Set keybind") == 0,
                   "Expected the shortcut capture modal");
            if (!capture || std::strcmp(capture->Name, "Set keybind") != 0) return;
            // Cancel is the last item. Use its rendered row bounds because
            // the dialog font is larger than the default test-context font.
            click(accept ? capture->DC.CursorStartPos.x + 25.f :
                           capture->DC.CursorPosPrevLine.x - 20.f,
                  capture->DC.CursorPosPrevLine.y + capture->DC.PrevLineSize.y * .5f);
            expect(popup() && std::strcmp(popup()->Name, "Settings") == 0 &&
                       ui::MixerWindowTestAccess::keybindTarget(mixer) == -1,
                   "Capture did not return to Settings");
        };
        auto closeKeybindSettings = [&](bool save) {
            if (auto *settings = popup())
                click(settings->DC.CursorStartPos.x + (save ? 633.f : 453.f),
                      settings->DC.CursorStartPos.y + 716);
        };
        const Keybind monitoringShortcut{MOD_CONTROL | MOD_ALT, 'M'};
        openKeybindSettings();
        clickKeybindRow(0);
        ui::MixerWindowTestAccess::captureKeybind(mixer, VK_LCONTROL, MOD_CONTROL);
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT, true);
        expect(ui::MixerWindowTestAccess::capturedKeybind(mixer) == Keybind{},
               "Modifier-only or repeated input assigned a shortcut");
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT);
        expect(ui::MixerWindowTestAccess::capturedKeybind(mixer) == monitoringShortcut,
               "Shortcut capture lost the pressed key or modifiers");
        expect(ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == Keybind{} &&
                   config.monitoringKeybind == Keybind{},
               "Typing a shortcut committed it before accepting the capture");
        ui::MixerWindowTestAccess::captureKeybind(mixer, VK_BACK, 0);
        expect(ui::MixerWindowTestAccess::capturedKeybind(mixer) == Keybind{},
               "Backspace did not clear the captured shortcut");
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT);
        ui::MixerWindowTestAccess::captureKeybind(mixer, VK_DELETE, 0);
        expect(ui::MixerWindowTestAccess::capturedKeybind(mixer) == Keybind{},
               "Delete did not clear the captured shortcut");
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT);
        ui::MixerWindowTestAccess::captureKeybind(mixer, VK_ESCAPE, 0);
        frame(1600, 986);
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0 &&
                   ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == Keybind{},
               "Escape saved a capture or closed the parent settings");
        clickKeybindRow(0);
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT);
        closeCapture(false);
        expect(ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == Keybind{},
               "Cancel committed a shortcut capture");
        clickKeybindRow(0);
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'K', MOD_CONTROL);
        ui::MixerWindowTestAccess::captureKeybind(mixer, VK_RETURN, 0);
        frame(1600, 986);
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0 &&
                   ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == Keybind{MOD_CONTROL, 'K'} &&
                   config.monitoringKeybind == Keybind{},
               "Enter did not accept a captured shortcut into the settings draft");
        clickKeybindRow(0);
        ui::MixerWindowTestAccess::captureKeybind(mixer, 'M', MOD_CONTROL | MOD_ALT);
        closeCapture(true);
        expect(ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == monitoringShortcut &&
                   config.monitoringKeybind == Keybind{},
               "Accepting a capture did not remain staged in Settings");
        closeKeybindSettings(false);
        expect(!popup() && config.monitoringKeybind == Keybind{},
               "Cancel Settings saved a staged shortcut");

        // Independent source/output bindings must not overwrite runtime mute
        // changes or refreshed endpoint IDs made while Settings remains open.
        ChannelConfig secondShortcutOutput;
        secondShortcutOutput.label = "Second shortcut output";
        secondShortcutOutput.deviceId = L"shortcut-output-two";
        expect(config.addOutput(secondShortcutOutput), "Could not add a second shortcut output");
        const Keybind firstSourceShortcut{MOD_CONTROL | MOD_ALT, '1'};
        const Keybind secondSourceShortcut{MOD_CONTROL | MOD_ALT, '2'};
        const Keybind firstOutputShortcut{MOD_CONTROL | MOD_ALT, '3'};
        const Keybind secondOutputShortcut{MOD_CONTROL | MOD_ALT, '4'};
        openKeybindSettings();
        auto assignShortcut = [&](int target, const Keybind &shortcut) {
            clickKeybindRow(target);
            ui::MixerWindowTestAccess::captureKeybind(mixer, shortcut.key, shortcut.modifiers);
            closeCapture(true);
        };
        assignShortcut(0, monitoringShortcut);
        assignShortcut(1, firstSourceShortcut);
        assignShortcut(2, secondSourceShortcut);
        assignShortcut(1 + kMaxSources, firstOutputShortcut);
        assignShortcut(2 + kMaxSources, secondOutputShortcut);
        config.sources[0].muted = true;
        config.sources[1].muted = false;
        config.outputAt(0).muted = true;
        config.outputAt(1).muted = false;
        config.outputAt(1).deviceId = L"newly-resolved-shortcut-output";
        closeKeybindSettings(true);
        expect(!popup() && config.monitoringKeybind == monitoringShortcut &&
                   config.sources[0].muteKeybind == firstSourceShortcut &&
                   config.sources[1].muteKeybind == secondSourceShortcut &&
                   config.outputAt(0).muteKeybind == firstOutputShortcut &&
                   config.outputAt(1).muteKeybind == secondOutputShortcut,
               "Save did not retain independent monitoring, source and output shortcuts");
        expect(config.sources[0].muted && !config.sources[1].muted &&
                   config.outputAt(0).muted && !config.outputAt(1).muted &&
                   config.outputAt(1).deviceId == L"newly-resolved-shortcut-output",
               "Saving shortcuts reverted live device state from the settings draft");
        openKeybindSettings();
        clickKeybindRow(0, true);
        expect(ui::MixerWindowTestAccess::settingsDraft(mixer).monitoringKeybind == Keybind{} &&
                   config.monitoringKeybind == monitoringShortcut,
               "Clear did not remain staged until Settings is saved");
        closeKeybindSettings(false);
        expect(config.monitoringKeybind == monitoringShortcut,
               "Cancel Settings saved a cleared shortcut");
        openKeybindSettings();
        clickKeybindRow(0, true);
        closeKeybindSettings(true);
        expect(!popup() && config.monitoringKeybind == Keybind{} &&
                   config.sources[0].muteKeybind == firstSourceShortcut,
               "Clearing monitoring also cleared an independent device shortcut");

        // Duplicate shortcuts are rejected without committing any preferences.
        openKeybindSettings();
        assignShortcut(0, firstSourceShortcut);
        ui::MixerWindowTestAccess::settingsDraft(mixer).bufferMillis = 123;
        const auto bufferBeforeDuplicate = config.bufferMillis;
        closeKeybindSettings(true);
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0 &&
                   config.monitoringKeybind == Keybind{} && config.bufferMillis == bufferBeforeDuplicate,
               "A duplicate shortcut partially committed the settings draft");
        closeKeybindSettings(false);

        // Exercise the same action dispatcher used by WM_HOTKEY, with a
        // deterministic running engine instead of reserving global OS keys.
        AudioEngineTestAccess::prepareMeterSession(engine, config);
        mixer.setVisible(true);
        for (const bool monitoring : {true, false}) {
            expect(ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::Monitoring),
                   "Monitoring shortcut action was rejected");
            expect(engine.running() && engine.monitoring() == monitoring,
                   "Monitoring shortcut restarted or stopped the shared capture session");
        }
        for (const bool muted : {true, false}) {
            expect(ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::SourceMute, 1),
                   "Source mute shortcut action was rejected");
            expect(config.sources[1].muted == muted &&
                       AudioEngineTestAccess::sourceMuted(engine, 1) == muted && config.sources[0].muted &&
                       config.outputAt(0).muted && !config.outputAt(1).muted,
                   "Source shortcut changed a different device or failed to toggle");
        }
        for (const bool muted : {true, false}) {
            expect(ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::OutputMute, 1),
                   "Output mute shortcut action was rejected");
            expect(config.outputAt(1).muted == muted &&
                       AudioEngineTestAccess::outputMuted(engine, 1) == muted && config.outputAt(0).muted &&
                       config.sources[0].muted && !config.sources[1].muted,
                   "Output shortcut changed a different device or failed to toggle");
        }
        expect(!ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::SourceMute,
                                                               config.sources.size()) &&
                   !ui::MixerWindowTestAccess::executeHotkeyAction(mixer, Hotkeys::ActionKind::OutputMute,
                                                                  config.outputCount()),
               "A stale shortcut action accessed a removed device");
        mixer.setVisible(false);

        ImGui::DestroyContext();
    }
    CoUninitialize();
    std::printf("Headless dashboard: %d failures\n", failed);
    return failed ? 1 : 0;
}
