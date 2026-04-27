#include "buffer_panel.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Elements/ElementTabSet.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Debugger.h>

#include <algorithm>
#include <fstream>

#include <ui/app_ui.hpp>

#include <fmt/format.h>
#include <efsw/efsw.hpp>

void replace_all(std::string &s, std::string const &toReplace, std::string const &replaceWith, bool wordBoundary = false);

struct BufferFileEditState {
    std::string name;
    std::filesystem::path path;
    std::atomic_bool modified = false;

    ~BufferFileEditState();
};

namespace {

    /** Shadertoy 新建着色器风格的默认片段（与官网空白工程常见模板一致） */
    static auto default_glsl_code_for_pass_type(std::string const &pass_type) -> std::string {
        if (pass_type == "common") {
            return "vec4 someFunction( vec4 a, float b )\n{\n    return a+b;\n}";
        }
        if (pass_type == "cubemap") {
            return "void mainCubemap( out vec4 fragColor, in vec2 fragCoord, in vec3 rayOri, in vec3 rayDir )\n{\n"
                   "    vec3 col = 0.5 + 0.5*rayDir;\n"
                   "    fragColor = vec4(col,1.0);\n"
                   "}\n";
        }
        /* buffer / image / sound / volume 等：mainImage 余弦色带 */
        return "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n"
               "    vec2 uv = fragCoord/iResolution.xy;\n"
               "    vec3 col = 0.5 + 0.5*cos(iTime+uv.xyx+vec3(0,2,4));\n"
               "    fragColor = vec4(col,1.0);\n"
               "}\n";
    }

    static auto resolve_pass_code(nlohmann::json &renderpass) -> std::string {
        std::string pass_type = "image";
        if (renderpass.contains("type") && renderpass["type"].is_string()) {
            pass_type = renderpass["type"].get<std::string>();
        }
        std::string code;
        if (renderpass.contains("code") && renderpass["code"].is_string()) {
            code = renderpass["code"].get<std::string>();
        }
        if (code.empty()) {
            code = default_glsl_code_for_pass_type(pass_type);
            renderpass["code"] = code;
        }
        return code;
    }

    class BufferPanelEventListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            switch (event.GetId()) {
            case Rml::EventId::Mouseup: {
                auto *element = event.GetTargetElement();
                auto const &element_id = element->GetId();
                if (element_id.starts_with("ichannel")) {
                    if (element_id == "ichannel0_img" ||
                        element_id == "ichannel1_img" ||
                        element_id == "ichannel2_img" ||
                        element_id == "ichannel3_img") {
                        // Show input selection window
                        AppUi::s_instance->buffer_panel.open_ichannel_img_element = element;
                        auto *bpiw_element = AppUi::s_instance->buffer_panel.bpiw_element;
                        bpiw_element->SetProperty("display", "block");
                        bpiw_element->Focus();
                    }
                }
            } break;
            default: break;
            }
        }
    };
    BufferPanelEventListener buffer_panel_event_listener;

    static auto element_is_or_contains_ancestor(Rml::Element *ancestor, Rml::Element *node) -> bool {
        if (ancestor == nullptr || node == nullptr) {
            return false;
        }
        for (auto *p = node; p != nullptr; p = p->GetParentNode()) {
            if (p == ancestor) {
                return true;
            }
        }
        return false;
    }

    class BpiwEventListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            switch (event.GetId()) {
            case Rml::EventId::Blur: {
                auto &panel = AppUi::s_instance->buffer_panel;
                if (panel.bpiw_dragging) {
                    break;
                }
                auto *bpiw_el = panel.bpiw_element;
                if (bpiw_el == nullptr) {
                    break;
                }
                /* 焦点移到 #bpiw 内部子节点时仍会触发 #bpiw 的 Blur；勿关闭，否则后续 onclick 无法应用选择。 */
                if (auto *doc = bpiw_el->GetOwnerDocument(); doc != nullptr) {
                    if (auto *ctx = doc->GetContext(); ctx != nullptr) {
                        if (element_is_or_contains_ancestor(bpiw_el, ctx->GetFocusElement())) {
                            break;
                        }
                    }
                }
                bpiw_el->SetProperty("display", "none");
                /* 勿在此清空 open_ichannel_img_element：Blur 常先于选项的 onclick，清空会导致无法写入对应 iChannel。 */
            } break;
            default: break;
            }
        }
    };
    BpiwEventListener bpiw_event_listener;

    static auto bpiw_target_is_close_tree(Rml::Element *target) -> bool {
        for (auto *el = target; el != nullptr; el = el->GetParentNode()) {
            if (el->GetId() == "bpiw_close") {
                return true;
            }
        }
        return false;
    }

    class BpiwHeaderDragListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            if (event.GetId() != Rml::EventId::Mousedown) {
                return;
            }
            if (event.GetParameter("button", 0) != 0) {
                return;
            }
            if (bpiw_target_is_close_tree(event.GetTargetElement())) {
                return;
            }
            auto &panel = AppUi::s_instance->buffer_panel;
            auto *bw = panel.bpiw_element;
            if (bw == nullptr) {
                return;
            }
            auto const pos = bw->GetAbsoluteOffset();
            auto const mx = event.GetParameter("mouse_x", 0.0f);
            auto const my = event.GetParameter("mouse_y", 0.0f);
            panel.bpiw_drag_grab_mx = mx - pos.x;
            panel.bpiw_drag_grab_my = my - pos.y;
            panel.bpiw_dragging = true;
        }
    };
    BpiwHeaderDragListener bpiw_header_drag_listener;

    class BpiwScreenDragListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            auto &panel = AppUi::s_instance->buffer_panel;
            switch (event.GetId()) {
            case Rml::EventId::Mousemove: {
                if (!panel.bpiw_dragging || panel.bpiw_element == nullptr) {
                    return;
                }
                auto *bw = panel.bpiw_element;
                auto *ctx = bw->GetContext();
                if (ctx == nullptr) {
                    return;
                }
                auto const mx = event.GetParameter("mouse_x", 0.0f);
                auto const my = event.GetParameter("mouse_y", 0.0f);
                float nl = mx - panel.bpiw_drag_grab_mx;
                float nt = my - panel.bpiw_drag_grab_my;
                auto const dim = ctx->GetDimensions();
                auto const sz = bw->GetBox().GetSize(Rml::Box::BORDER);
                float const max_x = float(dim.x) - sz.x;
                float const max_y = float(dim.y) - sz.y;
                nl = std::max(0.f, std::min(nl, max_x));
                nt = std::max(0.f, std::min(nt, max_y));
                auto const left_s = fmt::format("{:.0f}px", nl);
                auto const top_s = fmt::format("{:.0f}px", nt);
                (void)bw->SetProperty("left", Rml::String(left_s.c_str()));
                (void)bw->SetProperty("top", Rml::String(top_s.c_str()));
            } break;
            case Rml::EventId::Mouseup: {
                panel.bpiw_dragging = false;
            } break;
            default: break;
            }
        }
    };
    BpiwScreenDragListener bpiw_screen_drag_listener;

    class BufferPanelAddOptionsEventListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            switch (event.GetId()) {
            case Rml::EventId::Blur: {
                event.GetCurrentElement()->SetAttribute("style", "display: none;");
            } break;
            default: break;
            }
        }
    };
    BufferPanelAddOptionsEventListener buffer_panel_add_options_event_listener;

    auto random_string(size_t length) -> std::string {
        auto randchar = []() -> char {
            const char charset[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";
            const size_t max_index = (sizeof(charset) - 1);
            return charset[rand() % max_index];
        };
        std::string str(length, 0);
        std::generate_n(str.begin(), length, randchar);
        return str;
    }

    auto find_pass(std::string const &pass_name) -> nlohmann::json * {
        auto &renderpasses = AppUi::s_instance->buffer_panel.json["renderpass"];
        for (auto &renderpass : renderpasses) {
            auto name = std::string{};
            if (renderpass["name"] == pass_name) {
                return &renderpass;
            }
        }
        return nullptr;
    }

    auto find_input(nlohmann::json &pass_json, int channel_index) -> nlohmann::json * {
        for (auto &input : pass_json["inputs"]) {
            if (input["channel"] == channel_index) {
                return &input;
            }
        }
        return nullptr;
    }

    auto find_descendant_by_tag(Rml::Element *root, Rml::String const &tag) -> Rml::Element * {
        if (root == nullptr) {
            return nullptr;
        }
        if (root->GetTagName() == tag) {
            return root;
        }
        for (int i = 0; i < root->GetNumChildren(); ++i) {
            if (auto *found = find_descendant_by_tag(root->GetChild(i), tag)) {
                return found;
            }
        }
        return nullptr;
    }

    auto find_descendant_by_class(Rml::Element *root, Rml::String const &needle) -> Rml::Element * {
        if (root == nullptr) {
            return nullptr;
        }
        /* 勿用 substring：否则 "buffer_pass_code_editor" 会误匹配 "buffer_pass_code_editor_wrap" */
        if (root->IsClassSet(needle)) {
            return root;
        }
        for (int i = 0; i < root->GetNumChildren(); ++i) {
            if (auto *found = find_descendant_by_class(root->GetChild(i), needle)) {
                return found;
            }
        }
        return nullptr;
    }

    /** 点击 bpiw_option 内任意处（含文字说明区）时，需从 Current/Target 向上定位选项块再取预览图 src。 */
    auto bpiw_option_preview_img_from_click(Rml::Event &event) -> Rml::Element * {
        auto const walk = [](Rml::Element *start) -> Rml::Element * {
            for (auto *node = start; node != nullptr; node = node->GetParentNode()) {
                if (node->IsClassSet("bpiw_option")) {
                    return find_descendant_by_class(node, "bpiw_option_preview");
                }
            }
            return nullptr;
        };
        if (auto *img = walk(event.GetCurrentElement())) {
            return img;
        }
        return walk(event.GetTargetElement());
    }

    static constexpr size_t buffer_panel_pass_code_max_chars = 100000;

    auto sync_textarea_value_to_pass_json(Rml::ElementFormControlTextArea *ta) -> bool {
        if (ta == nullptr || AppUi::s_instance == nullptr) {
            return false;
        }
        auto *attr = ta->GetAttribute("data-pass");
        if (attr == nullptr) {
            return false;
        }
        auto pass_name = attr->Get<Rml::String>();
        auto name = std::string(pass_name.data(), pass_name.size());
        auto *pass_ptr = find_pass(name);
        if (pass_ptr == nullptr) {
            return false;
        }
        (*pass_ptr)["code"] = std::string(ta->GetValue());
        /* 不在此处设 dirty：仅在下方的「▶」编译时重新编译 GPU 管线，避免每次输入弹出编译错误 */
        return true;
    }

    void set_code_stats_for_textarea(Rml::ElementFormControlTextArea *ta) {
        if (ta == nullptr) {
            return;
        }
        /* textarea 在 .buffer_pass_code_editor_ta_cell 内，统计在并列的 toolbar 里 */
        auto *ta_cell = ta->GetParentNode();
        auto *wrap = ta_cell != nullptr ? ta_cell->GetParentNode() : nullptr;
        if (wrap == nullptr) {
            return;
        }
        auto *stats = find_descendant_by_class(wrap, "buffer_pass_code_stats");
        if (stats == nullptr) {
            return;
        }
        auto const n = std::string(ta->GetValue()).size();
        stats->SetInnerRML(Rml::String(fmt::format("{}/{}", n, buffer_panel_pass_code_max_chars).c_str()));
    }

    class PassCodeChangeListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            if (event.GetId() != Rml::EventId::Change) {
                return;
            }
            auto *ta = dynamic_cast<Rml::ElementFormControlTextArea *>(event.GetTargetElement());
            if (ta == nullptr) {
                return;
            }
            if (!sync_textarea_value_to_pass_json(ta)) {
                return;
            }
            set_code_stats_for_textarea(ta);
        }
    };
    PassCodeChangeListener pass_code_change_listener;

    class BufferPanelTabChangeListener : public Rml::EventListener {
      public:
        void ProcessEvent(Rml::Event &event) override {
            if (event.GetId() != Rml::EventId::Tabchange) {
                return;
            }
            if (AppUi::s_instance != nullptr) {
                AppUi::s_instance->buffer_panel.on_tab_changed();
            }
        }
    };
    BufferPanelTabChangeListener buffer_panel_tab_change_listener;

    class UpdateListener : public efsw::FileWatchListener {
      public:
        void handleFileAction([[maybe_unused]] efsw::WatchID watchid, [[maybe_unused]] const std::string &dir,
                              const std::string &filename, efsw::Action action,
                              [[maybe_unused]] std::string oldFilename) override {

            if (action != efsw::Actions::Modified) {
                // we don't care about anything except modifications
                return;
            }

            auto name = filename.substr(0, filename.find('_'));

            auto *edit_state_ptr = (BufferFileEditState *)nullptr;
            if (name == "Common") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.common_file_edit_state;
            } else if (name == "Buffer A") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.buffer00_file_edit_state;
            } else if (name == "Buffer B") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.buffer01_file_edit_state;
            } else if (name == "Buffer C") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.buffer02_file_edit_state;
            } else if (name == "Buffer D") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.buffer03_file_edit_state;
            } else if (name == "Cube A") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.cubemap00_file_edit_state;
            } else if (name == "Image") {
                edit_state_ptr = AppUi::s_instance->buffer_panel.image_file_edit_state;
            } else {
                return;
            }

            if (edit_state_ptr == nullptr) {
                return;
            }
            edit_state_ptr->modified = true;
        }
    };

    auto temp_directory = std::filesystem::temp_directory_path();
    std::unique_ptr<UpdateListener> update_listener = std::make_unique<UpdateListener>();
    std::unique_ptr<efsw::FileWatcher> file_watcher = []() {
        auto result = std::make_unique<efsw::FileWatcher>();
        result->addWatch(temp_directory.string(), update_listener.get());
        result->watch();
        return result;
    }();
    /** tab 内结构：tab → body.buffer_tab → .buffer_tab_row → [#content, button] */
    auto buffer_tab_row_from_tab(Rml::Element *tab) -> Rml::Element * {
        if (tab == nullptr || tab->GetNumChildren() < 1) {
            return nullptr;
        }
        auto *body = tab->GetChild(0);
        if (body == nullptr || body->GetNumChildren() < 1) {
            return nullptr;
        }
        return body->GetChild(0);
    }

    auto buffer_tab_content_div_from_tab(Rml::Element *tab) -> Rml::Element * {
        auto *row = buffer_tab_row_from_tab(tab);
        if (row == nullptr || row->GetNumChildren() < 1) {
            return nullptr;
        }
        return row->GetChild(0);
    }

    auto buffer_tab_close_button_from_tab(Rml::Element *tab) -> Rml::Element * {
        auto *row = buffer_tab_row_from_tab(tab);
        if (row == nullptr || row->GetNumChildren() < 2) {
            return nullptr;
        }
        return row->GetChild(1);
    }

    static auto trim_pass_name(std::string s) -> std::string {
        while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') {
            s.erase(0, 1);
        }
        while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') {
            s.pop_back();
        }
        return s;
    }

    /** 当前 buffer tab 对应的 renderpass 名称（与 JSON 中 name 一致）。模板下 #content 未必含 ElementText 子节点，故多路回退。 */
    auto active_pass_name_from_tabs(Rml::ElementTabSet *tabs_element) -> std::string {
        if (tabs_element == nullptr) {
            return {};
        }
        auto const active = tabs_element->GetActiveTab();
        auto *tab_row = tabs_element->GetChild(0);
        if (tab_row == nullptr || active < 0 || active >= tab_row->GetNumChildren()) {
            return {};
        }
        auto *tab_handle = tab_row->GetChild(active);
        if (auto *content_div = buffer_tab_content_div_from_tab(tab_handle); content_div != nullptr) {
            if (auto *et = dynamic_cast<Rml::ElementText *>(content_div->GetChild(0))) {
                return trim_pass_name(std::string(et->GetText()));
            }
            for (int i = 0; i < content_div->GetNumChildren(); ++i) {
                if (auto *et = dynamic_cast<Rml::ElementText *>(content_div->GetChild(i))) {
                    return trim_pass_name(std::string(et->GetText()));
                }
            }
            Rml::String inner;
            content_div->GetInnerRML(inner);
            if (!inner.empty()) {
                auto out = trim_pass_name(std::string(inner.c_str()));
                if (!out.empty()) {
                    return out;
                }
            }
        }
        auto *panels = tabs_element->GetChild(1);
        if (panels != nullptr && active >= 0 && active < panels->GetNumChildren()) {
            if (auto *ta = dynamic_cast<Rml::ElementFormControlTextArea *>(
                    find_descendant_by_class(panels->GetChild(active), "buffer_pass_code_editor"))) {
                if (auto *attr = ta->GetAttribute("data-pass"); attr != nullptr) {
                    auto const s = attr->Get<Rml::String>();
                    if (!s.empty()) {
                        return trim_pass_name(std::string(s.data(), s.size()));
                    }
                }
            }
        }
        return {};
    }
} // namespace

BufferFileEditState::~BufferFileEditState() {
    // ignore error code.
    auto ec = std::error_code{};
    std::filesystem::remove(path, ec);
}

BufferPanel::~BufferPanel() {
    cleanup();
}

void BufferPanel::load([[maybe_unused]] Rml::Context *rml_context, Rml::ElementDocument *document) {
    base_element = document->GetElementById("buffer_panel");
    base_element->AddEventListener(Rml::EventId::Mousedown, &buffer_panel_event_listener);
    base_element->AddEventListener(Rml::EventId::Mouseup, &buffer_panel_event_listener);
    base_element->AddEventListener(Rml::EventId::Mousemove, &buffer_panel_event_listener);
    base_element->AddEventListener(Rml::EventId::Keydown, &buffer_panel_event_listener);
    base_element->AddEventListener(Rml::EventId::Keyup, &buffer_panel_event_listener);

    bpiw_element = document->GetElementById("bpiw");
    bpiw_element->AddEventListener(Rml::EventId::Blur, &bpiw_event_listener);

    if (auto *bpiw_header = document->GetElementById("bpiw_header"); bpiw_header != nullptr) {
        bpiw_header->AddEventListener(Rml::EventId::Mousedown, &bpiw_header_drag_listener);
    }
    if (auto *screen_el = document->GetElementById("screen"); screen_el != nullptr) {
        screen_el->AddEventListener(Rml::EventId::Mousemove, &bpiw_screen_drag_listener);
        screen_el->AddEventListener(Rml::EventId::Mouseup, &bpiw_screen_drag_listener);
    }

    auto *buffer_panel_add_panel = document->GetElementById("buffer_panel_add_panel");
    buffer_panel_add_panel->AddEventListener(Rml::EventId::Blur, &buffer_panel_add_options_event_listener);

    tabs_element = dynamic_cast<Rml::ElementTabSet *>(document->GetElementById("buffer_tabs"));
    tabs_element->AddEventListener(Rml::EventId::Tabchange, &buffer_panel_tab_change_listener);
}

void BufferPanel::process_event(Rml::Event &event, std::string const &value) {
    if (during_shader_load) {
        return;
    }

    auto const active_pass_name = active_pass_name_from_tabs(tabs_element);

    if (value == "buffer_panel_ichannel_settings") {
        buffer_panel_ichannel_settings(event);
    } else if (value == "buffer_panel_bpiw_close") {
        buffer_panel_bpiw_close();
    } else if (value == "buffer_panel_change_filter") {
        buffer_panel_change_filter(event, active_pass_name);
    } else if (value == "buffer_panel_change_wrap") {
        buffer_panel_change_wrap(event, active_pass_name);
    } else if (value == "buffer_panel_bpiw_select") {
        buffer_panel_bpiw_select(event, active_pass_name);
    } else if (value == "buffer_panel_ichannel_close") {
        buffer_panel_ichannel_close(event, active_pass_name);
    } else if (value == "buffer_panel_add") {
        buffer_panel_add(event);
    } else if (value == "buffer_panel_add_option") {
        buffer_panel_add_option(event);
    } else if (value == "buffer_panel_tab_close") {
        buffer_panel_tab_close(event);
    } else if (value == "buffer_panel_compile_shader") {
        buffer_panel_compile_shader(event);
    } else if (value == "buffer_panel_load_json") {
        buffer_panel_load_json();
    } else if (value == "buffer_panel_shader_input_toggle") {
        buffer_panel_shader_input_toggle(event);
    }
}

void BufferPanel::buffer_panel_ichannel_settings(Rml::Event &event) {
    auto *ichannel = event.GetCurrentElement()->GetParentNode()->GetParentNode()->GetChild(0);
    auto *ichannel_img = ichannel->GetChild(0);
    auto *ichannel_sampler_menu = ichannel->GetChild(1);

    auto is_sampler_menu_closed = ichannel_sampler_menu->GetAttribute("style")->Get(Rml::String("display: none;")) == "display: none;";
    if (is_sampler_menu_closed) {
        ichannel_img->SetAttribute("style", "display: none; image-color: #000000;");
        ichannel_sampler_menu->SetAttribute("style", "display: inline-block;");
    } else {
        ichannel_img->SetAttribute("style", "display: inline-block; image-color: #ffffff;");
        ichannel_sampler_menu->SetAttribute("style", "display: none;");
    }
}
void BufferPanel::buffer_panel_bpiw_close() {
    bpiw_dragging = false;
    open_ichannel_img_element = nullptr;
    bpiw_element->SetProperty("display", "none");
    bpiw_element->Blur();
}
void BufferPanel::buffer_panel_change_filter(Rml::Event &event, std::string const &active_pass_name) {
    auto *filter_select = dynamic_cast<Rml::ElementFormControlSelect *>(event.GetCurrentElement());
    if (filter_select == nullptr || active_pass_name.empty()) {
        return;
    }
    auto *pass_ptr = find_pass(active_pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;
    auto *ichannel = filter_select->GetParentNode()->GetParentNode();
    auto ichannel_name = ichannel->GetId();
    auto channel_index = int(ichannel_name[8]) - int('0');
    auto *channel_input = find_input(pass, channel_index);
    if (channel_input != nullptr) {
        auto &input = *channel_input;
        input["sampler"]["filter"] = filter_select->GetValue();
        dirty = true;
    }
}
void BufferPanel::buffer_panel_change_wrap(Rml::Event &event, std::string const &active_pass_name) {
    auto *wrap_select = dynamic_cast<Rml::ElementFormControlSelect *>(event.GetCurrentElement());
    if (wrap_select == nullptr || active_pass_name.empty()) {
        return;
    }
    auto *pass_ptr = find_pass(active_pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;
    auto *ichannel = wrap_select->GetParentNode()->GetParentNode();
    auto ichannel_name = ichannel->GetId();
    auto channel_index = int(ichannel_name[8]) - int('0');
    auto *channel_input = find_input(pass, channel_index);
    if (channel_input != nullptr) {
        auto &input = *channel_input;
        input["sampler"]["wrap"] = wrap_select->GetValue();
        dirty = true;
    }
}
void BufferPanel::buffer_panel_bpiw_select(Rml::Event &event, std::string const &active_pass_name) {
    auto *select_img = bpiw_option_preview_img_from_click(event);
    if (select_img == nullptr || open_ichannel_img_element == nullptr || active_pass_name.empty()) {
        return;
    }
    auto *src_attr = select_img->GetAttribute("src");
    if (src_attr == nullptr) {
        return;
    }
    auto img_path = src_attr->Get(Rml::String(""));

    auto *pass_ptr = find_pass(active_pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;

    auto *bpiw_tabs = dynamic_cast<Rml::ElementTabSet *>(bpiw_element->GetElementById("bpiw_tabs"));
    if (bpiw_tabs == nullptr) {
        return;
    }

    auto *datagrid_column = open_ichannel_img_element->GetParentNode()->GetParentNode();
    auto *ichannel = datagrid_column->GetChild(0);
    auto *ichannel_label = datagrid_column->GetChild(1);
    auto *ichannel_label_settings = ichannel_label->GetChild(1);

    auto *ichannel_img = ichannel->GetChild(0);
    auto *ichannel_sampler_menu = ichannel->GetChild(1);
    auto *ichannel_close = ichannel->GetChild(2);

    ichannel_img->SetAttribute("src", img_path);
    ichannel_img->SetAttribute("style", "display: inline-block; image-color: #ffffff;");
    ichannel_close->SetAttribute("style", "display: inline-block;");
    ichannel_sampler_menu->SetAttribute("style", "display: none;");
    ichannel_label_settings->SetAttribute("style", "display: block;");

    auto ichannel_name = ichannel->GetId();
    auto channel_index = int(ichannel_name[8]) - int('0');

    replace_all(img_path, "../../media/images/", "/media/a/");
    auto default_sampler = nlohmann::json{};
    default_sampler["filter"] = "linear";
    default_sampler["wrap"] = "clamp";
    default_sampler["vflip"] = "true";
    default_sampler["srgb"] = "false";
    default_sampler["internal"] = "byte";

    auto new_input = nlohmann::json{};
    new_input["id"] = std::to_string(std::hash<std::string>{}(img_path));
    new_input["channel"] = channel_index;
    new_input["sampler"] = default_sampler;
    new_input["published"] = 1;

    switch (bpiw_tabs->GetActiveTab()) {
    case 0: {
        if (img_path == "../../media/icons/keyboard.png") {
            new_input["filepath"] = "/presets/tex00.jpg";
            new_input["type"] = "keyboard";
            new_input["sampler"]["filter"] = "nearest";
        } else if (img_path == "../../media/icons/buffer00.png") {
            new_input["id"] = "4dXGR8";
            new_input["filepath"] = "/media/previz/buffer00.png";
            new_input["type"] = "buffer";
        } else if (img_path == "../../media/icons/buffer01.png") {
            new_input["id"] = "XsXGR8";
            new_input["filepath"] = "/media/previz/buffer01.png";
            new_input["type"] = "buffer";
        } else if (img_path == "../../media/icons/buffer02.png") {
            new_input["id"] = "4sXGR8";
            new_input["filepath"] = "/media/previz/buffer02.png";
            new_input["type"] = "buffer";
        } else if (img_path == "../../media/icons/buffer03.png") {
            new_input["id"] = "XdfGR8";
            new_input["filepath"] = "/media/previz/buffer03.png";
            new_input["type"] = "buffer";
        } else if (img_path == "../../media/icons/cubemap00.png") {
            new_input["id"] = "4dX3Rr";
            new_input["filepath"] = "/media/previz/cubemap00.png";
            new_input["type"] = "cubemap";
        }
    } break;
    case 1: {
        new_input["filepath"] = img_path;
        new_input["type"] = "texture";
        new_input["sampler"]["filter"] = "mipmap";
        new_input["sampler"]["wrap"] = "repeat";
    } break;
    case 2: {
        new_input["filepath"] = img_path;
        new_input["type"] = "cubemap";
    } break;
    case 3: {
        if (img_path == "../../media/icons/volume_gray.png") {
            new_input["id"] = "4sfGRr";
            new_input["filepath"] = "/media/a/27012b4eadd0c3ce12498b867058e4f717ce79e10a99568cca461682d84a4b04.bin";
            new_input["type"] = "volume";
        } else if (img_path == "../../media/icons/buffer00.png") {
            new_input["id"] = "XdX3Rr";
            new_input["filepath"] = "/media/a/aea6b99da1d53055107966b59ac5444fc8bc7b3ce2d0bbb6a4a3cbae1d97f3aa.bin";
            new_input["type"] = "volume";
        }
    } break;
    }

    auto *channel_input = find_input(pass, channel_index);
    if (channel_input != nullptr) {
        *channel_input = new_input;
    } else {
        pass["inputs"].push_back(new_input);
    }

    bpiw_dragging = false;
    open_ichannel_img_element = nullptr;
    bpiw_element->SetProperty("display", "none");
    bpiw_element->Blur();
    dirty = true;
}
void BufferPanel::buffer_panel_ichannel_close(Rml::Event &event, std::string const &active_pass_name) {
    if (active_pass_name.empty()) {
        return;
    }
    auto *pass_ptr = find_pass(active_pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;

    auto *datagrid_column = event.GetCurrentElement()->GetParentNode()->GetParentNode();
    auto *ichannel = datagrid_column->GetChild(0);

    auto ichannel_name = ichannel->GetId();
    auto channel_index = int(ichannel_name[8]) - int('0');

    int current_index = 0;
    for (auto &input : pass["inputs"]) {
        if (input["channel"] == channel_index) {
            auto *ichannel_label = datagrid_column->GetChild(1);
            auto *ichannel_label_settings = ichannel_label->GetChild(1);

            auto *ichannel_img = ichannel->GetChild(0);
            auto *ichannel_sampler_menu = ichannel->GetChild(1);
            auto *ichannel_close = ichannel->GetChild(2);

            ichannel_img->SetAttribute("style", "display: inline-block; image-color: #000000;");
            ichannel_img->SetAttribute("src", "");
            ichannel_close->SetAttribute("style", "display: none;");
            ichannel_sampler_menu->SetAttribute("style", "display: none;");
            ichannel_label_settings->SetAttribute("style", "display: none;");

            pass["inputs"].erase(current_index);
            dirty = true;
            break;
        }
        ++current_index;
    }
}
void BufferPanel::buffer_panel_add(Rml::Event &event) {
    auto *buffer_panel_add_panel = event.GetCurrentElement()->GetElementById("buffer_panel_add_panel");
    buffer_panel_add_panel->SetAttribute("style", "display: inline-block;");
    buffer_panel_add_panel->Focus();

    auto *buffer_panel_add_common = buffer_panel_add_panel->GetElementById("buffer_panel_add_common");
    auto *buffer_panel_add_buffer00 = buffer_panel_add_panel->GetElementById("buffer_panel_add_buffer00");
    auto *buffer_panel_add_buffer01 = buffer_panel_add_panel->GetElementById("buffer_panel_add_buffer01");
    auto *buffer_panel_add_buffer02 = buffer_panel_add_panel->GetElementById("buffer_panel_add_buffer02");
    auto *buffer_panel_add_buffer03 = buffer_panel_add_panel->GetElementById("buffer_panel_add_buffer03");
    auto *buffer_panel_add_cubemap00 = buffer_panel_add_panel->GetElementById("buffer_panel_add_cubemap00");

    auto add_option_to_list = [&](auto &&name, auto *option_element) {
        auto *channel_input = find_pass(name);
        if (channel_input != nullptr) {
            // pass already exists, hide it
            option_element->SetAttribute("style", "display: none;");
        } else {
            // show it in the list
            option_element->SetAttribute("style", "display: block;");
        }
    };

    add_option_to_list("Common", buffer_panel_add_common);
    add_option_to_list("Buffer A", buffer_panel_add_buffer00);
    add_option_to_list("Buffer B", buffer_panel_add_buffer01);
    add_option_to_list("Buffer C", buffer_panel_add_buffer02);
    add_option_to_list("Buffer D", buffer_panel_add_buffer03);
    add_option_to_list("Cube A", buffer_panel_add_cubemap00);
}
void BufferPanel::buffer_panel_add_option(Rml::Event &event) {
    auto *buffer_panel_add_panel = event.GetCurrentElement()->GetElementById("buffer_panel_add_panel");
    buffer_panel_add_panel->SetAttribute("style", "display: none;");
    buffer_panel_add_panel->Blur();

    // Add the buffer to the passes
    // NOTE(grundlett): I'll just modify the json and reload it...

    auto &renderpasses = json["renderpass"];

    auto new_pass = nlohmann::json{};
    auto element_id = event.GetCurrentElement()->GetId();

    auto iter = renderpasses.begin();

    if (element_id != "buffer_panel_add_common") {
        // find image pass
        while (iter != renderpasses.end()) {
            // Buffer passes should always be before the cube/image passes.
            if ((*iter)["name"] == "Image" || (*iter)["name"] == "Cube A") {
                break;
            }
            ++iter;
        }
    }

    if (iter == renderpasses.end()) {
        // should never happen
        return;
    }

    if (element_id == "buffer_panel_add_common") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [],
                "inputs": [],
                "code": "vec4 someFunction( vec4 a, float b )\n{\n    return a+b;\n}",
                "name": "Common",
                "description": "",
                "type": "common"
            })");
    } else if (element_id == "buffer_panel_add_buffer00") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [
                    {
                        "channel": 0,
                        "id": "4dXGR8"
                    }
                ],
                "inputs": [],
                "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
                "name": "Buffer A",
                "description": "",
                "type": "buffer"
            })");
    } else if (element_id == "buffer_panel_add_buffer01") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [
                    {
                        "channel": 0,
                        "id": "XsXGR8"
                    }
                ],
                "inputs": [],
                "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
                "name": "Buffer B",
                "description": "",
                "type": "buffer"
            })");
    } else if (element_id == "buffer_panel_add_buffer02") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [
                    {
                        "channel": 0,
                        "id": "4sXGR8"
                    }
                ],
                "inputs": [],
                "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
                "name": "Buffer C",
                "description": "",
                "type": "buffer"
            })");
    } else if (element_id == "buffer_panel_add_buffer03") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [
                    {
                        "channel": 0,
                        "id": "XdfGR8"
                    }
                ],
                "inputs": [],
                "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
                "name": "Buffer D",
                "description": "",
                "type": "buffer"
            })");
    } else if (element_id == "buffer_panel_add_cubemap00") {
        new_pass = nlohmann::json::parse(R"({
                "outputs": [
                    {
                        "channel": 0,
                        "id": "4dX3Rr"
                    }
                ],
                "inputs": [],
                "code": "void mainCubemap( out vec4 fragColor, in vec2 fragCoord, in vec3 rayOri, in vec3 rayDir )\n{\n    // Ray direction as color\n    vec3 col = 0.5 + 0.5*rayDir;\n\n    // Output to cubemap\n    fragColor = vec4(col,1.0);\n}",
                "name": "Cube A",
                "description": "",
                "type": "cubemap"
            })");
    }

    auto tab_index = static_cast<int32_t>(iter - renderpasses.begin());

    renderpasses.insert(iter, new_pass);
    reload_json();

    tabs_element->SetActiveTab(tab_index);
}
void BufferPanel::buffer_panel_tab_close(Rml::Event &event) {
    auto &renderpasses = json["renderpass"];

    auto new_pass = nlohmann::json{};
    auto *element = event.GetCurrentElement();
    auto *parent = element->GetParentNode();
    auto *content_div = parent->GetChild(0);
    auto *tab_content =
        content_div != nullptr ? dynamic_cast<Rml::ElementText *>(content_div->GetChild(0)) : nullptr;
    if (tab_content == nullptr) {
        return;
    }

    auto name = tab_content->GetText();

    auto iter = renderpasses.begin();
    while (iter != renderpasses.end()) {
        if ((*iter)["name"] == name) {
            break;
        }
        ++iter;
    }

    if (iter == renderpasses.end()) {
        // Should never happen
        return;
    }

    renderpasses.erase(iter);
    // Cope because we are about to rebuild the UI while it's propagating the events
    event.StopImmediatePropagation();
    reload_json();
}

void BufferPanel::buffer_panel_compile_shader(Rml::Event &) {
    sync_all_panel_textareas_to_json();
    dirty = true;
    update_active_tab_code_stats_display();
}

void BufferPanel::buffer_panel_load_json() {
    if (AppUi::s_instance != nullptr) {
        AppUi::s_instance->load_json();
    }
}

void BufferPanel::on_tab_changed() {
    if (during_shader_load) {
        return;
    }
    sync_all_panel_textareas_to_json();
    update_active_tab_code_stats_display();
}

void BufferPanel::sync_all_panel_textareas_to_json() {
    if (tabs_element == nullptr) {
        return;
    }
    auto *panels = tabs_element->GetChild(1);
    for (int i = 0; i < panels->GetNumChildren(); ++i) {
        auto *panel = panels->GetChild(i);
        auto *ta = dynamic_cast<Rml::ElementFormControlTextArea *>(find_descendant_by_class(panel, "buffer_pass_code_editor"));
        if (ta == nullptr) {
            continue;
        }
        auto *attr = ta->GetAttribute("data-pass");
        if (attr == nullptr) {
            continue;
        }
        auto pass_name = attr->Get<Rml::String>();
        auto name = std::string(pass_name.data(), pass_name.size());
        auto *pass_ptr = find_pass(name);
        if (pass_ptr == nullptr || !(*pass_ptr).contains("code") || !(*pass_ptr)["code"].is_string()) {
            continue;
        }
        auto &pass = *pass_ptr;
        auto new_code = std::string(ta->GetValue());
        if (pass["code"].get<std::string>() != new_code) {
            pass["code"] = new_code;
        }
    }
}

void BufferPanel::update_active_tab_code_stats_display() {
    if (tabs_element == nullptr) {
        return;
    }
    auto const idx = tabs_element->GetActiveTab();
    auto *panels = tabs_element->GetChild(1);
    if (idx < 0 || idx >= panels->GetNumChildren()) {
        return;
    }
    auto *ta = dynamic_cast<Rml::ElementFormControlTextArea *>(
        find_descendant_by_class(panels->GetChild(idx), "buffer_pass_code_editor"));
    if (ta != nullptr) {
        set_code_stats_for_textarea(ta);
    }
}

void BufferPanel::buffer_panel_shader_input_toggle(Rml::Event &event) {
    Rml::Element *node = event.GetCurrentElement();
    Rml::Element *header = nullptr;
    while (node != nullptr) {
        if (node->GetClassNames().find("shader_input_header") != Rml::String::npos) {
            header = node;
            break;
        }
        node = node->GetParentNode();
    }
    if (header == nullptr) {
        return;
    }
    Rml::Element *wrap = header->GetParentNode();
    if (wrap == nullptr || wrap->GetNumChildren() < 2) {
        return;
    }
    Rml::Element *body = wrap->GetChild(1);
    Rml::Element *chevron = nullptr;
    if (auto *first = header->GetChild(0); first != nullptr) {
        if (first->GetClassNames().find("shader_input_chevron_cell") != Rml::String::npos &&
            first->GetNumChildren() > 0) {
            chevron = first->GetChild(0);
        } else if (first->GetClassNames().find("shader_input_chevron") != Rml::String::npos) {
            chevron = first;
        }
    }
    if (chevron == nullptr) {
        return;
    }
    auto const display = body->GetProperty("display")->ToString();
    if (display == "none") {
        body->SetProperty("display", "block");
        chevron->SetAttribute("class", "shader_input_chevron shader_input_chevron_expanded");
    } else {
        body->SetProperty("display", "none");
        chevron->SetAttribute("class", "shader_input_chevron shader_input_chevron_collapsed");
    }
}

void BufferPanel::cleanup() {
    delete common_file_edit_state;
    delete buffer00_file_edit_state;
    delete buffer01_file_edit_state;
    delete buffer02_file_edit_state;
    delete buffer03_file_edit_state;
    delete cubemap00_file_edit_state;
    delete image_file_edit_state;

    common_file_edit_state = nullptr;
    buffer00_file_edit_state = nullptr;
    buffer01_file_edit_state = nullptr;
    buffer02_file_edit_state = nullptr;
    buffer03_file_edit_state = nullptr;
    cubemap00_file_edit_state = nullptr;
    image_file_edit_state = nullptr;
}

void BufferPanel::update() {
    auto code_changed = false;

    auto update_edit_state = [&](BufferFileEditState *edit_state_ptr) {
        if (edit_state_ptr == nullptr) {
            return;
        }
        if (!edit_state_ptr->modified.load()) {
            return;
        }

        // if it was modified, we want to update the shader.

        auto new_content = [&]() {
            auto file = std::ifstream(edit_state_ptr->path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }();

        if (new_content.empty()) {
            return;
        }

        auto *pass_ptr = find_pass(edit_state_ptr->name);
        if (pass_ptr == nullptr) {
            // Shouldn't ever happen
            return;
        }
        auto &pass = *pass_ptr;
        auto old_content = std::string(pass["code"]);
        if (new_content != old_content) {
            pass["code"] = new_content;
            code_changed = true;
        }
    };

    update_edit_state(common_file_edit_state);
    update_edit_state(buffer00_file_edit_state);
    update_edit_state(buffer01_file_edit_state);
    update_edit_state(buffer02_file_edit_state);
    update_edit_state(buffer03_file_edit_state);
    update_edit_state(cubemap00_file_edit_state);
    update_edit_state(image_file_edit_state);

    if (code_changed) {
        AppUi::s_instance->buffer_panel.reload_json();
    }
}

void BufferPanel::load_shadertoy_json(nlohmann::json const &temp_json) {
    if (temp_json.contains("numShaders")) {
        // Is a "export all shaders" json file. Let's split it up for the user.
        for (auto &shader : temp_json["shaders"]) {
            auto filepath = std::string{"shader_"} + std::string{shader["info"]["id"]} + std::string{".json"};
            auto f = std::ofstream(filepath);
            f << std::setw(4) << shader;
        }

        json = temp_json["shaders"][0];
    } else if (temp_json.contains("Shader")) {
        json = temp_json["Shader"];
    } else {
        json = temp_json;
    }
    cleanup();
    reload_json();
}
void BufferPanel::reload_json() {
    during_shader_load = true;

    auto &renderpasses = json["renderpass"];

    auto tab_index = 0;

    auto tab_count = tabs_element->GetNumTabs();
    for (auto tab_i = 0; tab_i < tab_count; ++tab_i) {
        tabs_element->RemoveTab(tab_count - tab_i - 1);
    }

    for (auto &renderpass : renderpasses) {
        auto name = std::string{renderpass["name"]};

        if (name == "Image") {
            tabs_element->SetTab(tab_index, fmt::format("<template src=\"buffer_tab_image\">{}</template>", name));
        } else {
            tabs_element->SetTab(tab_index, fmt::format("<template src=\"buffer_tab\">{}</template>", name));
        }
        tabs_element->SetPanel(tab_index, "<template src=\"buffer_panel\"> </template>");

        auto *panels = tabs_element->GetChild(1);
        auto *panel = panels->GetChild(tab_index);
        auto *datagrid = find_descendant_by_tag(panel, "datagrid");
        if (datagrid == nullptr) {
            ++tab_index;
            continue;
        }
        auto *datagrid_header = datagrid->GetChild(0);

        auto *editor_wrap = find_descendant_by_class(panel, "buffer_pass_code_editor_wrap");
        auto *ta_el = find_descendant_by_class(panel, "buffer_pass_code_editor");
        auto *pass_code_ta = dynamic_cast<Rml::ElementFormControlTextArea *>(ta_el);

        auto &inputs = renderpass["inputs"];

        if (renderpass["type"] == "common") {
            datagrid->SetAttribute("style", "display: none;");
            if (editor_wrap != nullptr) {
                editor_wrap->SetAttribute("style", "display: none;");
            }
        } else {
            if (editor_wrap != nullptr) {
                editor_wrap->RemoveAttribute("style");
            }
            if (pass_code_ta != nullptr) {
                pass_code_ta->SetAttribute("data-pass", Rml::String(name.c_str()));
                pass_code_ta->AddEventListener(Rml::EventId::Change, &pass_code_change_listener);
                auto code = resolve_pass_code(renderpass);
                replace_all(code, "\\n", "\n");
                pass_code_ta->SetValue(Rml::String(code.c_str()));
                set_code_stats_for_textarea(pass_code_ta);
            }
            // buffer_panel.rml：每通道一列后接 buffer_ch_spacer，故 0→0、1→2、2→4、3→6
            for (auto channel_i = 0; channel_i < 4; ++channel_i) {
                auto *datagrid_column = datagrid_header->GetChild(channel_i * 2);
                auto *ichannel = datagrid_column->GetChild(0);
                auto *ichannel_label = datagrid_column->GetChild(1);
                auto *ichannel_label_settings = ichannel_label->GetChild(1);

                auto *ichannel_img = ichannel->GetChild(0);
                auto *ichannel_sampler_menu = ichannel->GetChild(1);
                auto *ichannel_close = ichannel->GetChild(2);

                bool has_input = false;

                for (auto &input : inputs) {
                    if (input["channel"] == channel_i) {
                        auto path = std::string{};
                        if (input.contains("filepath")) {
                            path = input["filepath"];
                        } else if (input.contains("src")) {
                            path = input["src"];
                        } else {
                            // ?
                            continue;
                        }
                        replace_all(path, "/media/a/", "media/images/");

                        auto input_type = std::string{};
                        if (input.contains("type")) {
                            input_type = input["type"];
                        } else if (input.contains("ctype")) {
                            input_type = input["ctype"];
                        } else {
                            // ?
                            continue;
                        }

                        if (std::filesystem::exists(path)) {
                            ichannel_img->SetAttribute("src", fmt::format("../../{}", path));
                            has_input = true;
                        } else if (input_type == "keyboard") {
                            ichannel_img->SetAttribute("src", "../../media/icons/keyboard.png");
                            has_input = true;
                        } else if (input_type == "buffer") {
                            auto buffer_index = 0;
                            for (auto &input_renderpass : renderpasses) {
                                if (input_renderpass["type"] == "buffer") {
                                    if (input_renderpass["outputs"][0]["id"] == input["id"]) {
                                        break;
                                    }
                                    ++buffer_index;
                                }
                            }

                            ichannel_img->SetAttribute("src", fmt::format("../../media/icons/buffer0{}.png", buffer_index));
                            has_input = true;
                        } else if (input_type == "cubemap") {
                            ichannel_img->SetAttribute("src", "../../media/icons/cubemap00.png");
                            has_input = true;
                        }

                        if (input.contains("sampler")) {
                            auto &sampler = input["sampler"];
                            auto *filter_select = dynamic_cast<Rml::ElementFormControlSelect *>(ichannel_sampler_menu->GetChild(1));
                            auto *wrap_select = dynamic_cast<Rml::ElementFormControlSelect *>(ichannel_sampler_menu->GetChild(4));
                            auto filter = std::string{"linear"};
                            auto wrap = std::string{"repeat"};
                            if (sampler.contains("filter")) {
                                filter = std::string(sampler["filter"]);
                            }
                            if (sampler.contains("wrap")) {
                                wrap = std::string(sampler["wrap"]);
                            }
                            filter_select->SetValue(filter);
                            wrap_select->SetValue(wrap);
                        }
                    }
                }

                if (has_input) {
                    ichannel_img->SetAttribute("style", "display: inline-block; image-color: #ffffff;");
                    ichannel_close->SetAttribute("style", "display: inline-block;");
                    ichannel_sampler_menu->SetAttribute("style", "display: none;");
                    ichannel_label_settings->SetAttribute("style", "display: block;");
                } else {
                    ichannel_img->SetAttribute("style", "display: inline-block; image-color: #000000;");
                    ichannel_img->SetAttribute("src", "");
                    ichannel_close->SetAttribute("style", "display: none;");
                    ichannel_sampler_menu->SetAttribute("style", "display: none;");
                    ichannel_label_settings->SetAttribute("style", "display: none;");
                }
            }

            if (name == "Image") {
                tabs_element->SetActiveTab(tab_index);
            }
        }

        ++tab_index;
    }

    dirty = true;
    during_shader_load = false;
    update_active_tab_code_stats_display();
}
