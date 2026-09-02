// Exercise the actual ImGui dashboard without opening a desktop window or audio
// devices. This catches layout bounds and popup/control regressions headlessly.
#include "ui/MixerWindow.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <imgui_internal.h>
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
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        unsigned char *pixels;
        int fw, fh;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
        ui::applyTheme();
        mixer.init(&engine, &config, nullptr);
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
            io.AddMouseButtonEvent(0, true);
            frame(1600, 986);
            io.AddMouseButtonEvent(0, false);
            frame(1600, 986);
            frame(1600, 986);
        };
        frame(1600, 986);
        click(1200, 488); // Channels
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            std::printf("Popup missing at frame %d\n",ImGui::GetFrameCount()); ++failed;
        }
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame(1600, 986);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame(1600, 986);
        click(670, 900); // Settings
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            std::printf("Popup missing at frame %d\n",ImGui::GetFrameCount()); ++failed;
        }
        ImGui::DestroyContext();
    }
    CoUninitialize();
    std::printf("Headless dashboard: %d failures\n", failed);
    return failed ? 1 : 0;
}
