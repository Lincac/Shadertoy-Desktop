#pragma once

#include <functional>
#include <memory>
#include <span>

#include <daxa/daxa.hpp>
#include <GLFW/glfw3.h>

#include <ui/layout.hpp>

struct AppWindow {
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> glfw_window{nullptr, &glfwDestroyWindow};
    daxa::Swapchain swapchain{};
    daxa_i32vec2 size{};

    struct FullscreenCache {
        daxa_i32vec2 pos{};
        daxa_i32vec2 size{};
    };

    FullscreenCache fullscreen_cache{};

    ui_layout::Layout layout{};
    bool layout_fullscreen = false;

    std::function<void()> on_resize{};
    std::function<void()> on_close{};
    std::function<void(float, float)> on_mouse_move{};
    std::function<void(int32_t, int32_t)> on_mouse_button{};
    std::function<void(int32_t, int32_t)> on_key{};
    std::function<void(std::span<char const *>)> on_drop{};

    /** 返回 true 表示事件未被 ImGui 消费，可转发给 viewport */
    std::function<bool(int glfw_key, int glfw_action, int glfw_mods)> on_global_key{};

    AppWindow() = default;
    explicit AppWindow(daxa::Device device, daxa_i32vec2 size);

    void update();
    void set_fullscreen(bool is_fullscreen);
    void set_vsync(bool enabled);

    [[nodiscard]] auto should_forward_viewport_input() const -> bool;

    void sync_layout_from_window();
};
