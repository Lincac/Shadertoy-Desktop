#pragma once

#include <imgui.h>

namespace ui_theme {

inline ImVec4 const k_panel_bg{0.824f, 0.824f, 0.824f, 1.f};           // #d2d2d2
inline ImVec4 const k_tab_inactive{0.502f, 0.502f, 0.502f, 1.f};       // #808080
inline ImVec4 const k_tab_active{0.973f, 0.690f, 0.188f, 1.f};         // #F8B030
inline ImVec4 const k_tab_hover{0.690f, 0.502f, 0.376f, 1.f};          // #B08060
inline ImVec4 const k_editor_bg{0.965f, 0.965f, 0.965f, 1.f};          // #f6f6f6
inline ImVec4 const k_editor_text{0.067f, 0.067f, 0.067f, 1.f};        // #111
inline ImVec4 const k_editor_border{0.659f, 0.659f, 0.659f, 1.f};      // #a8a8a8
inline ImVec4 const k_toolbar_bg{0.784f, 0.784f, 0.784f, 1.f};          // #c8c8c8
inline ImVec4 const k_bottom_bar_bg{1.f, 1.f, 1.f, 1.f};
inline ImVec4 const k_bottom_bar_text{0.f, 0.f, 0.f, 1.f};
inline ImVec4 const k_shader_input_header{0.769f, 0.769f, 0.769f, 1.f}; // #c4c4c4
inline ImVec4 const k_shader_input_body{0.902f, 0.902f, 0.902f, 1.f};   // #e6e6e6

inline auto push_buffer_panel_style() -> int {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k_panel_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, k_editor_text);
    ImGui::PushStyleColor(ImGuiCol_Tab, k_tab_inactive);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, k_tab_hover);
    ImGui::PushStyleColor(ImGuiCol_TabActive, k_tab_active);
    ImGui::PushStyleColor(ImGuiCol_TabUnfocused, k_tab_inactive);
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, k_tab_active);
    return 7;
}

inline void pop_buffer_panel_style(int count) {
    ImGui::PopStyleColor(count);
}

inline auto push_code_editor_style() -> int {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, k_editor_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, k_editor_text);
    ImGui::PushStyleColor(ImGuiCol_Border, k_editor_border);
    return 3;
}

inline void pop_code_editor_style(int count) {
    ImGui::PopStyleColor(count);
}

inline auto push_bottom_bar_style() -> int {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k_bottom_bar_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, k_bottom_bar_text);
    ImGui::PushStyleColor(ImGuiCol_Button, k_bottom_bar_bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.92f, 0.92f, 0.92f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.85f, 0.85f, 0.85f, 1.f});
    return 5;
}

inline void pop_bottom_bar_style(int count) {
    ImGui::PopStyleColor(count);
}

} // namespace ui_theme
