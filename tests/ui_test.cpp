// Exercise the actual ImGui dashboard without opening a desktop window or audio
// devices. This catches layout bounds and popup/control regressions headlessly.
#include "ui/MixerWindow.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
using namespace audiomon;
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
        click(1200, 488); // Channels
        expect(popup() != nullptr, "Channels menu missing");
        expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Native dragging steals input from a menu");
        if (auto *menu = popup()) {
            // Select the last row (Mono), then immediately use another control.
            click(menu->DC.CursorStartPos.x + 20, menu->DC.CursorMaxPos.y - 10);
        }
        expect(config.mono && !popup(), "Selecting Mono did not finish the menu action");
        click(850, 785); // Master volume
        expect(config.output.gain < .9f, "Volume blocked after choosing channels");
        click(670, 900); // Settings
        expect(popup() != nullptr, "Settings blocked after adjusting volume");
        expect(!mixer.hitTitleBar(700, 50, 1600, 986), "Native dragging steals input from a modal");
        if (auto *dialog = popup()) {
            click(dialog->DC.CursorStartPos.x + 20, dialog->DC.CursorPosPrevLine.y + 10); // Done
        }
        expect(!popup(), "Done did not close Settings");
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Title bar stays blocked after closing Settings");
        click(1200, 488); // Channels again, with no Escape between any actions.
        expect(popup() != nullptr, "Channels blocked after closing Settings");
        if (auto *menu = popup())
            click(menu->DC.CursorStartPos.x + 20, menu->DC.CursorStartPos.y + 10); // Stereo
        expect(!config.mono && !popup(), "Selecting Stereo did not finish the menu action");
        click(1440, 488); // Status
        expect(popup() != nullptr, "Status menu missing");
        click(700, 50); // Dismiss the menu by clicking the title bar.
        expect(!popup(), "Title-bar click did not dismiss Status");
        expect(mixer.hitTitleBar(700, 50, 1600, 986), "Title bar stays blocked after dismissing Status");
        click(670, 900);
        expect(popup() != nullptr, "Settings blocked after dismissing Status");
        auto expectPopupFits = [&](ImGuiWindow *dialog) {
            expect(dialog && dialog->Pos.x >= 0 && dialog->Pos.y >= 0 &&
                       dialog->Pos.x + dialog->Size.x <= io.DisplaySize.x &&
                       dialog->Pos.y + dialog->Size.y <= io.DisplaySize.y,
                   "Popup extends beyond the application");
        };
        auto dragAndResizePopup = [&]() {
            auto *dialog = popup();
            if (!dialog) { expect(false, "Missing dialog for drag test"); return; }
            const ImVec2 original = dialog->Pos;
            const ImVec2 grab{dialog->Pos.x + dialog->Size.x / 2,
                              dialog->Pos.y + dialog->TitleBarHeight / 2};
            io.AddMousePosEvent(grab.x, grab.y);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, true);
            frame(1600, 986);
            io.AddMousePosEvent(grab.x + 20, grab.y + 20);
            frame(1600, 986);
            expect(dialog->Pos.x > original.x && dialog->Pos.y > original.y,
                   "Popup can no longer be dragged within the application");
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
        dragAndResizePopup(); // Settings
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 110, dialog->DC.CursorPosPrevLine.y + 10); // Restore defaults
        expect(popup() && std::strcmp(popup()->Name, "Restore defaults?") == 0,
               "Confirmation popup missing");
        dragAndResizePopup(); // Nested confirmation, without resetting anything.
        if (auto *dialog = popup())
            click(dialog->DC.CursorPosPrevLine.x - 20, dialog->DC.CursorPosPrevLine.y + 10); // Cancel
        if (auto *dialog = popup())
            click(dialog->DC.CursorStartPos.x + 20, dialog->DC.CursorPosPrevLine.y + 10); // Done
        expect(!popup(), "Settings remained open after drag tests");
        click(440, 239); // Configure the first source.
        expect(popup() && std::strcmp(popup()->Name, "Configure source") == 0, "Source popup missing");
        dragAndResizePopup();
        ImGui::DestroyContext();
    }
    CoUninitialize();
    std::printf("Headless dashboard: %d failures\n", failed);
    return failed ? 1 : 0;
}
