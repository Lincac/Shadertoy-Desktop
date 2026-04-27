#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace Rml {
    class Context;
    class ElementDocument;
    class Event;
    class Element;
    class ElementTabSet;
} // namespace Rml

struct BufferFileEditState;

struct BufferPanel {
    nlohmann::json json;
    bool dirty = false;
    bool during_shader_load = false;

    Rml::Element *base_element{};
    Rml::ElementTabSet *tabs_element{};
    Rml::Element *bpiw_element{};
    Rml::Element *open_ichannel_img_element{};
    /** iChannel 选择弹窗：标题栏拖拽 */
    bool bpiw_dragging = false;
    float bpiw_drag_grab_mx = 0.f;
    float bpiw_drag_grab_my = 0.f;

    BufferFileEditState *common_file_edit_state{};
    BufferFileEditState *buffer00_file_edit_state{};
    BufferFileEditState *buffer01_file_edit_state{};
    BufferFileEditState *buffer02_file_edit_state{};
    BufferFileEditState *buffer03_file_edit_state{};
    BufferFileEditState *cubemap00_file_edit_state{};
    BufferFileEditState *image_file_edit_state{};

    ~BufferPanel();

    void load(Rml::Context *rml_context, Rml::ElementDocument *document);
    void process_event(Rml::Event &event, std::string const &value);
    void cleanup();
    void update();

    void load_shadertoy_json(nlohmann::json const &temp_json);
    void reload_json();
    [[nodiscard]] auto get_shadertoy_json() const -> auto const & {
        return json;
    }

    /** Tab 切换：把各 tab 文本框写回 JSON 并刷新当前 tab 字数统计 */
    void on_tab_changed();

  private:
    void buffer_panel_ichannel_settings(Rml::Event &event);
    void buffer_panel_bpiw_close();
    void buffer_panel_change_filter(Rml::Event &event, std::string const &active_pass_name);
    void buffer_panel_change_wrap(Rml::Event &event, std::string const &active_pass_name);
    void buffer_panel_bpiw_select(Rml::Event &event, std::string const &active_pass_name);
    void buffer_panel_ichannel_close(Rml::Event &event, std::string const &active_pass_name);
    void buffer_panel_add(Rml::Event &event);
    void buffer_panel_add_option(Rml::Event &event);
    void buffer_panel_tab_close(Rml::Event &event);
    void buffer_panel_compile_shader(Rml::Event &event);
    void buffer_panel_load_json();
    void buffer_panel_shader_input_toggle(Rml::Event &event);
    void sync_all_panel_textareas_to_json();
    void update_active_tab_code_stats_display();
};
