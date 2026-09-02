#include "ui/Theme.h"
#include <imgui.h>
namespace audiomon::ui {
void applyTheme() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 18;
    s.ChildRounding = 16;
    s.PopupRounding = 14;
    s.FrameRounding = 7;
    s.GrabRounding = 7;
    s.ScrollbarRounding = 8;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(22, 22);
    s.FramePadding = ImVec2(12, 9);
    s.ItemSpacing = ImVec2(12, 12);
    s.GrabMinSize = 18;
    auto *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(.045f, .06f, .07f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(.075f, .095f, .11f, 1);
    c[ImGuiCol_Border] = ImVec4(.16f, .19f, .21f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(.12f, .14f, .16f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(.18f, .20f, .24f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(.22f, .19f, .34f, 1);
    c[ImGuiCol_Button] = ImVec4(.16f, .13f, .25f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.29f, .22f, .45f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.42f, .28f, .68f, 1);
    c[ImGuiCol_Header] = c[ImGuiCol_Button];
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_SliderGrab] = ImVec4(.53f, .36f, 1, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(.68f, .55f, 1, 1);
    c[ImGuiCol_CheckMark] = ImVec4(.6f, .43f, 1, 1);
    c[ImGuiCol_Text] = ImVec4(.94f, .95f, .97f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.63f, .67f, .71f, 1);
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_PopupBg];
}
} // namespace audiomon::ui
