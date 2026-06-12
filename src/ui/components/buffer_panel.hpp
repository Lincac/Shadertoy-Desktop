#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <daxa/daxa.hpp>
#include <nlohmann/json.hpp>

struct BufferPanel {
    nlohmann::json json;
    bool dirty = false;

    int active_tab = 0;
    bool show_shader_input = false;

    /** pass 名称 → 编辑器缓冲（与 json 同步） */
    std::unordered_map<std::string, std::vector<char>> pass_code_buffers;

    /** iChannel 选择弹窗 */
    bool show_bpiw = false;
    int bpiw_target_channel = 0;
    std::string bpiw_pass_name;
    std::string compile_message;
    bool show_compile_error_dialog = false;
    std::function<void()> on_recompile;

    /** 各通道是否显示 sampler 设置（相对当前 pass） */
    std::array<bool, 4> channel_show_sampler{};

    static inline constexpr size_t pass_code_max_chars = 100000;

    void load_shadertoy_json(nlohmann::json const &temp_json);
    void reload_json();
    [[nodiscard]] auto get_shadertoy_json() const -> auto const & {
        return json;
    }

    /** 绘制右侧 buffer 面板（ImGui） */
    void draw(float panel_width, float panel_height);

    /** 同步当前 tab 代码到 json */
    void sync_active_pass_code_to_json();
    /** 同步全部 pass 代码到 json */
    void sync_all_pass_codes_to_json();

    void compile_shader();
    void load_json_from_dialog();

    /** 释放 BPIW 预览纹理等 GPU 资源（须在 Device 销毁前调用） */
    static void shutdown_gpu_resources(daxa::Device device);

  private:
    void rebuild_code_buffers_from_json();
    void sort_renderpasses();
    void sync_pass_code_to_json(std::string const &pass_name);
    [[nodiscard]] auto pass_name_at(int tab_index) const -> std::string;
    [[nodiscard]] auto active_pass_name() const -> std::string;
    [[nodiscard]] auto active_pass_is_common() const -> bool;
    [[nodiscard]] auto pass_count() const -> int;

    void draw_tabs_and_add();
    void draw_shader_input_section();
    void draw_code_editor(float editor_height);
    void draw_code_toolbar();
    void draw_channels();
    void draw_bpiw_modal();
    void draw_compile_error_modal();

    void add_pass(std::string const &pass_kind);
    void close_tab(int tab_index);
    void apply_bpiw_selection(std::string const &img_path, int bpiw_category);
    void remove_channel_input(std::string const &pass_name, int channel_index);
};
