#include "ui/Theme.h"
#include <imgui.h>

namespace audiomon::ui {

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 0.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowBorderSize  = 0.0f;
    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 7);
    s.GrabMinSize       = 14.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.129f, 0.133f, 0.145f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.157f, 0.161f, 0.176f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.110f, 0.114f, 0.125f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.208f, 0.216f, 0.235f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.259f, 0.271f, 0.294f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.302f, 0.318f, 0.345f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.157f, 0.161f, 0.176f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.227f, 0.235f, 0.255f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.290f, 0.302f, 0.325f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.353f, 0.369f, 0.396f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.545f, 0.573f, 0.639f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.686f, 0.714f, 0.780f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.239f, 0.247f, 0.267f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.878f, 0.886f, 0.902f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.451f, 0.463f, 0.486f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.400f, 0.750f, 0.450f, 1.00f);
}

} // namespace audiomon::ui
