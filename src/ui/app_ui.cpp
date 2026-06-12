#include <ui/app_ui.hpp>

#include <app/resources.hpp>
#include <ui/ui_theme.hpp>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fmt/format.h>
#include <nfd.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdlib>
#include <fstream>

AppUi::AppUi(daxa::Device device_in)
    : device{device_in},
      app_window(device, daxa_i32vec2{800 + 760, 450 + 22 + 2 + 189 + 2}) {
    assert(s_instance == nullptr);
    s_instance = this;

    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(app_window.glfw_window.get(), false);
    setup_imgui_style();

    imgui_renderer.emplace(daxa::ImGuiRendererInfo{
        .device = device,
        .format = app_window.swapchain.get_format(),
        .context = ImGui::GetCurrentContext(),
    });

    icons.load(resource_dir / "src/ui/icon", device);

    app_window.on_close = [&]() { should_close.store(true); };

    app_window.on_global_key = [this](int glfw_key, int glfw_action, int glfw_mods) {
        return handle_global_key(glfw_key, glfw_action, glfw_mods);
    };
}

AppUi::~AppUi() {
    shutdown();
}

void AppUi::shutdown() {
    if (gpu_shutdown_done) {
        return;
    }
    gpu_shutdown_done = true;

    if (imgui_renderer.has_value()) {
        imgui_renderer.reset();
    }
    buffer_panel.shutdown_gpu_resources(device);
    icons.destroy(device);
    app_window.swapchain = {};
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    device = {};
}

void AppUi::setup_imgui_style() {
    ImGui::StyleColorsLight();
    auto &style = ImGui::GetStyle();
    style.WindowRounding = 0.f;
    style.FrameRounding = 0.f;
    style.TabRounding = 0.f;
    style.WindowBorderSize = 0.f;
    style.ScrollbarSize = 14.f;
    style.WindowPadding = ImVec2{4.f, 4.f};
    style.FramePadding = ImVec2{4.f, 2.f};
    style.ItemSpacing = ImVec2{4.f, 2.f};

    auto const font_path = (resource_dir / "media/fonts/LatoLatin-Regular.ttf").string();
    if (std::filesystem::exists(font_path)) {
        ImGuiIO &io = ImGui::GetIO();
        ui_font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 15.f);
        for (size_t i = 0; i < k_code_font_sizes.size(); ++i) {
            code_fonts[i] = io.Fonts->AddFontFromFileTTF(font_path.c_str(), k_code_font_sizes[i]);
        }
        code_font = code_fonts[static_cast<size_t>(code_font_size_index)];
        io.FontDefault = ui_font;
    }
}

auto AppUi::get_layout() const -> ui_layout::Layout {
    return app_window.layout;
}

void AppUi::update(float time, float fps) {
    display_time = time;
    display_fps = fps;
    app_window.sync_layout_from_window();
    app_window.update();

    if (pending_fullscreen_toggle) {
        pending_fullscreen_toggle = false;
        toggle_fullscreen();
    }

    if (is_fullscreen) {
        return;
    }

    if (imgui_renderer.has_value()) {
        icons.refresh_texture_ids(*imgui_renderer);
    }

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    build_ui();
}

void AppUi::build_ui() {
    auto const ui_h = ImGui::GetIO().DisplaySize.y;
    draw_bottom_bar();
    buffer_panel.draw(ui_layout::k_buffer_panel_width, ui_h);
    draw_download_dialog();
    draw_settings_dialog();
}

void AppUi::draw_bottom_bar() {
    auto const layout = get_layout();
    auto const bar_h = ui_layout::k_bottom_bar_height;
    auto const bar_w = layout.viewport_width();
    auto const display_h = ImGui::GetIO().DisplaySize.y;
    auto const icon_size = ImVec2{ui_layout::k_bottom_bar_icon_size, ui_layout::k_bottom_bar_icon_size};
    auto const icon_y = (bar_h - icon_size.y) * 0.5f;
    auto const icon_tint = ImVec4{0.f, 0.f, 0.f, 1.f};

    ImGui::SetNextWindowPos(ImVec2{0.f, display_h - bar_h}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2{bar_w, bar_h}, ImGuiCond_Always);

    int const style_count = ui_theme::push_bottom_bar_style();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.f, 0.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.f, 0.f});
    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("BottomBar", nullptr, flags)) {
        ImGui::SetCursorPos(ImVec2{16.f, icon_y});
        if (icons.image_button("##reset", icons.reset, icon_size, icon_tint)) {
            if (on_reset) {
                on_reset();
            }
        }

        ImGui::SetCursorPos(ImVec2{44.f, icon_y});
        auto const &pause_icon = paused ? icons.play : icons.pause;
        if (icons.image_button("##pause", pause_icon, icon_size, icon_tint)) {
            paused = !paused;
            if (on_toggle_pause) {
                on_toggle_pause(paused);
            }
        }

        ImGui::SetCursorPos(ImVec2{96.f, icon_y + 2.f});
        ImGui::Text("%.2f", display_time);
        ImGui::SetCursorPos(ImVec2{160.f, icon_y + 2.f});
        ImGui::Text("%.1f fps", display_fps);
        ImGui::SetCursorPos(ImVec2{250.f, icon_y + 2.f});
        ImGui::Text("%d x %d", static_cast<int>(layout.viewport_width()), static_cast<int>(layout.viewport_height()));

        ImGui::SetCursorPos(ImVec2{bar_w - 16.f - ui_layout::k_bottom_bar_icon_size, icon_y});
        if (icons.image_button("##fullscreen", icons.fullscreen, icon_size, icon_tint)) {
            request_toggle_fullscreen();
        }
        ImGui::SetCursorPos(ImVec2{bar_w - 48.f - ui_layout::k_bottom_bar_icon_size, icon_y});
        if (icons.image_button("##settings", icons.settings, icon_size, icon_tint)) {
            show_settings_dialog = !show_settings_dialog;
        }
        ImGui::SetCursorPos(ImVec2{bar_w - 80.f - ui_layout::k_bottom_bar_icon_size, icon_y});
        if (icons.image_button("##download", icons.download, icon_size, icon_tint)) {
            show_download_dialog = !show_download_dialog;
        }
        ImGui::SetCursorPos(ImVec2{bar_w - 112.f - ui_layout::k_bottom_bar_icon_size, icon_y});
        if (icons.image_button("##save", icons.save, icon_size, icon_tint)) {
            save_json(false);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ui_theme::pop_bottom_bar_style(style_count);
}

void AppUi::draw_download_dialog() {
    if (!show_download_dialog) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2{420, 80}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Download Shadertoy", &show_download_dialog)) {
        ImGui::InputText("URL or ID", download_input, sizeof(download_input));
        if (ImGui::Button("Download") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (on_download) {
                on_download(std::string{download_input});
            }
            show_download_dialog = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            show_download_dialog = false;
        }
    }
    ImGui::End();
}

void AppUi::draw_settings_dialog() {
    if (!show_settings_dialog) {
        return;
    }
    if (ImGui::Begin("Settings", &show_settings_dialog)) {
        bool vsync = true;
        if (ImGui::Checkbox("Vsync", &vsync)) {
            app_window.set_vsync(vsync);
        }
        ImGui::Checkbox("Export downloads to test-shader.json", &settings.export_downloads);
    }
    ImGui::End();
}

void AppUi::render(daxa::CommandRecorder &recorder, daxa::ImageId target_image) {
    ImGui::Render();
    auto const &display_size = ImGui::GetIO().DisplaySize;
    imgui_renderer->record_commands(
        ImGui::GetDrawData(),
        recorder,
        target_image,
        static_cast<daxa::u32>(display_size.x),
        static_cast<daxa::u32>(display_size.y));
}

void AppUi::request_toggle_fullscreen() {
    pending_fullscreen_toggle = true;
}

void AppUi::toggle_fullscreen() {
    is_fullscreen = !is_fullscreen;
    if (on_toggle_fullscreen) {
        on_toggle_fullscreen(is_fullscreen);
    } else {
        app_window.set_fullscreen(is_fullscreen);
    }
}

auto AppUi::handle_global_key(int glfw_key, int glfw_action, int glfw_mods) -> bool {
    if (glfw_action != GLFW_PRESS) {
        return true;
    }

    bool const ctrl = (glfw_mods & GLFW_MOD_CONTROL) != 0;
    bool const shift = (glfw_mods & GLFW_MOD_SHIFT) != 0;

    if (ctrl && glfw_key == GLFW_KEY_S) {
        save_json(shift);
        return false;
    }
    if (glfw_key == GLFW_KEY_F11) {
        request_toggle_fullscreen();
        return false;
    }
    if (glfw_key == GLFW_KEY_ESCAPE && is_fullscreen) {
        request_toggle_fullscreen();
        return false;
    }

    return true;
}

void AppUi::save_json(bool save_as) {
    buffer_panel.sync_all_pass_codes_to_json();
    if (!current_save_path || save_as) {
        nfdchar_t *out_path = nullptr;
        nfdresult_t const result = NFD_SaveDialog("json", "", &out_path);
        if (result == NFD_OKAY) {
            current_save_path = out_path;
            std::free(out_path);
        } else {
            current_save_path = std::nullopt;
            return;
        }
    }
    auto file = std::ofstream(*current_save_path);
    file << buffer_panel.get_shadertoy_json();
}

void AppUi::set_code_font_size(int index) {
    if (index < 0 || index >= static_cast<int>(k_code_font_sizes.size())) {
        return;
    }
    code_font_size_index = index;
    code_font = code_fonts[static_cast<size_t>(code_font_size_index)];
}

void AppUi::load_json() {
    nfdchar_t *out_path = nullptr;
    nfdresult_t const result = NFD_OpenDialog("json", nullptr, &out_path);
    if (result != NFD_OKAY || out_path == nullptr) {
        return;
    }
    std::filesystem::path const path(out_path);
    std::free(out_path);

    auto ifs = std::ifstream(path);
    if (!ifs) {
        return;
    }
    try {
        auto parsed = nlohmann::json::parse(ifs);
        buffer_panel.load_shadertoy_json(std::move(parsed));
        current_save_path = path;
    } catch (...) {
    }
}
