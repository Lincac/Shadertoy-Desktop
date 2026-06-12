#pragma once

#include <imgui.h>

namespace ui_layout {

inline constexpr float k_buffer_panel_width = 760.f;
inline constexpr float k_bottom_bar_height = 26.f;
inline constexpr float k_bottom_bar_icon_size = 22.f;
inline constexpr float k_code_toolbar_height = 28.f;
inline constexpr float k_ichannel_slot_height = 128.f;
inline constexpr float k_ichannel_label_height = 26.f;
inline constexpr float k_ichannel_row_height = k_ichannel_slot_height + k_ichannel_label_height + 2.f;
inline constexpr float k_channel_col_width = 179.f;
inline constexpr float k_channel_col_spacer = 14.f;
inline constexpr float k_tab_orange_border = 4.f;
inline constexpr float k_code_toolbar_corner_radius = 3.f;
inline constexpr float k_bpiw_window_w = 684.f;
inline constexpr float k_bpiw_window_h = 560.f;
inline constexpr float k_bpiw_card_w = 210.f;
inline constexpr float k_bpiw_card_h = 80.f;
inline constexpr float k_bpiw_preview_size = 70.f;
inline constexpr float k_bpiw_col_gap = 10.f;
inline constexpr int k_bpiw_cols = 3;

struct Layout {
    float window_w = 0.f;
    float window_h = 0.f;

    [[nodiscard]] auto viewport_width() const -> float {
        return window_w - k_buffer_panel_width;
    }

    [[nodiscard]] auto viewport_height() const -> float {
        return window_h - k_bottom_bar_height;
    }

    [[nodiscard]] auto viewport_pos() const -> ImVec2 {
        return {0.f, 0.f};
    }

    [[nodiscard]] auto viewport_size() const -> ImVec2 {
        return {viewport_width(), viewport_height()};
    }

    [[nodiscard]] auto point_in_viewport(float x, float y) const -> bool {
        return x >= 0.f && x < viewport_width() && y >= 0.f && y < viewport_height();
    }
};

} // namespace ui_layout
