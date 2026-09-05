// Exercise the actual ImGui dashboard without opening a desktop window or audio
// devices. This catches layout bounds and popup/control regressions headlessly.
#include "ui/MixerWindow.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace audiomon;

namespace audiomon::ui {
struct MixerWindowTestAccess {
    static void resetDropoutState(MixerWindow &mixer) {
        mixer.status_.clear();
        mixer.statusDetail_.clear();
        mixer.severity_ = 0;
        mixer.dropoutTimer_ = 0;
        mixer.lastUnderruns_ = 0;
        mixer.lastDropped_ = 0;
    }
    static void refreshDropoutStatus(MixerWindow &mixer, StreamState outputState,
                                     uint64_t underruns, uint64_t dropped) {
        mixer.statusDetail_.clear();
        mixer.severity_ = 0;
        mixer.refreshDropoutStatus(outputState, underruns, dropped, 1.f / 60.f);
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
};
} // namespace audiomon::ui

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failed = 0;
    {
        AudioEngine engine;
        Config config = Config::defaults();
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
        auto frame = [&](int w, int h) {
            io.DisplaySize = {float(w), float(h)};
            io.DeltaTime = 1.f / 60;
            ImGui::NewFrame();
            mixer.draw(io.DeltaTime, w, h);
            ImGui::Render();
            if (!ImGui::GetDrawData() || ImGui::GetDrawData()->TotalVtxCount == 0) {
                std::printf("No draw data at %dx%d frame %d\n",w,h,ImGui::GetFrameCount()); ++failed;
            }
        };

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
        expect(ImGui::GetStyle().Colors[ImGuiCol_NavCursor].w == 0.f,
               "Native navigation cursor is still visible under the custom focus ring");

        for (auto size : {ImVec2(1600, 986), ImVec2(1440, 890), ImVec2(960, 600), ImVec2(2200, 1200)}) {
            frame(int(size.x), int(size.y));
            frame(int(size.x), int(size.y));
            const float scale = std::min(size.x / 1600.f, size.y / 986.f);
            expect(mixer.hitTitleBar(int(700 * scale), int(50 * scale), int(size.x), int(size.y)),
                   "Title bar does not route dragging to Windows");
            expect(!mixer.hitTitleBar(int(size.x - 140 * scale), int(50 * scale), int(size.x), int(size.y)),
                   "Title bar overlaps window buttons");
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
        expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Native dragging steals input from Channels");
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
        expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Native dragging steals input from a modal");
        if (auto *dialog = popup()) {
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
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 83, dialog->DC.CursorStartPos.y + 716); // Restore defaults
        expect(popup() && std::strcmp(popup()->Name, "Restore defaults?") == 0,
               "Confirmation popup missing");
        dragAndResizePopup(); // Nested confirmation.
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 20, dialog->DC.CursorPosPrevLine.y + 10); // Reset
        expect(popup() && std::strcmp(popup()->Name, "Settings") == 0,
               "Reset did not return to Settings");
        expect(config.output.volume < .9f, "Restore defaults applied before Save");
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 633, dialog->DC.CursorStartPos.y + 716); // Save Settings
        expect(!popup() && config.output.volume == 1.f, "Restore defaults was not committed by Save");

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
                   config.output.gain == 4.f && config.output.volume == 1.f,
               "Typed output gain was not committed");
        expect(config.sources.size() == sourceCountBeforeOutputEdit,
               "Editing output accidentally changed the source list");
        holdVolumeSlider(1030, 785, config.output.volume, .5f);
        expect(config.output.gain == 4.f,
               "Master volume changed the configured output mix gain");

        config.sources[0].volume = 1.f;
        click(440, 239); // Configure the first source.
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0, "Source popup missing");
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

        auto pressKey = [&](ImGuiKey key) {
            io.AddKeyEvent(key, true);
            frame(1600, 986);
            io.AddKeyEvent(key, false);
            frame(1600, 986);
        };
        auto focusDashboardItem = [&](const char *label) {
            frame(1600, 986);
            auto *dashboard = ImGui::FindWindowByName("Audio Monitor dashboard");
            expect(dashboard != nullptr, "Dashboard missing for keyboard test");
            if (!dashboard)
                return;
            ImGui::SetNavWindow(dashboard);
            ImGui::SetNavID(dashboard->GetID(label), ImGuiNavLayer_Main,
                            dashboard->NavRootFocusScopeId, ImRect());
            ImGui::GetCurrentContext()->NavCursorVisible = true;
            frame(1600, 986);
        };

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
        click(260, 909);
        expect(popup() != nullptr, "Source limit popup did not open at the source cap");
        pressKey(ImGuiKey_Escape);
        expect(!popup(), "Escape did not close the Source limit popup");
        config.sources = savedSources;

        ImGui::DestroyContext();
    }
    CoUninitialize();
    std::printf("Headless dashboard: %d failures\n", failed);
    return failed ? 1 : 0;
}
