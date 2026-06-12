#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <daxa/daxa.hpp>
#include <daxa/utils/imgui.hpp>

#include <imgui.h>

#include <ui/app_window.hpp>
#include <ui/components/buffer_panel.hpp>
#include <ui/layout.hpp>
#include <ui/ui_icons.hpp>

struct AppSettings {
    bool export_downloads = false;
};

struct AppUi {
    std::atomic_bool should_close = false;
    daxa::Device device{};
    AppWindow app_window;
    std::optional<daxa::ImGuiRenderer> imgui_renderer;
    BufferPanel buffer_panel{};

    static inline AppUi *s_instance = nullptr;
    bool paused = false;
    bool is_fullscreen = false;
    AppSettings settings{};

    bool show_download_dialog = false;
    bool show_settings_dialog = false;
    char download_input[512]{};

    float display_time = 0.f;
    float display_fps = 0.f;

    std::function<void()> on_reset{};
    std::function<void(bool)> on_toggle_pause{};
    std::function<void(bool)> on_toggle_fullscreen{};
    std::function<void(std::string const &)> on_download{};

    std::optional<std::filesystem::path> current_save_path = std::nullopt;

    UiIcons icons{};
    ImFont *ui_font = nullptr;
    ImFont *code_font = nullptr;
    static inline constexpr std::array<float, 6> k_code_font_sizes = {10.f, 12.f, 14.f, 16.f, 18.f, 20.f};
    std::array<ImFont *, k_code_font_sizes.size()> code_fonts{};
    int code_font_size_index = 1;

    explicit AppUi(daxa::Device device);
    ~AppUi();

    AppUi(const AppUi &) = delete;
    AppUi(AppUi &&) = delete;
    auto operator=(const AppUi &) -> AppUi & = delete;
    auto operator=(AppUi &&) -> AppUi & = delete;

    void update(float time, float fps);
    void build_ui();
    void render(daxa::CommandRecorder &recorder, daxa::ImageId target_image);

    [[nodiscard]] auto get_layout() const -> ui_layout::Layout;

    void toggle_fullscreen();
    void request_toggle_fullscreen();
    void save_json(bool save_as);
    void load_json();
    void set_code_font_size(int index);
    /** 在 Device 销毁前释放 ImGui GPU 资源 */
    void shutdown();

  private:
    void setup_imgui_style();
    void draw_bottom_bar();
    void draw_download_dialog();
    void draw_settings_dialog();
    bool handle_global_key(int glfw_key, int glfw_action, int glfw_mods);

    bool pending_fullscreen_toggle = false;
    bool gpu_shutdown_done = false;
};
