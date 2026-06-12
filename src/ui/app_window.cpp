#include <ui/app_window.hpp>
#include <ui/layout.hpp>

#include <app/resources.hpp>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <daxa/c/core.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

auto get_native_handle(GLFWwindow *glfw_window_ptr) -> daxa::NativeWindowHandle {
#if defined(_WIN32)
    return glfwGetWin32Window(glfw_window_ptr);
#elif defined(__linux__)
    return reinterpret_cast<daxa::NativeWindowHandle>(glfwGetX11Window(glfw_window_ptr));
#endif
}

auto get_native_platform(GLFWwindow * /*unused*/) -> daxa::NativeWindowPlatform {
#if defined(_WIN32)
    return daxa::NativeWindowPlatform::WIN32_API;
#elif defined(__linux__)
    return daxa::NativeWindowPlatform::XLIB_API;
#endif
}

namespace {
    auto create(daxa_i32vec2 size) -> GLFWwindow * {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        return glfwCreateWindow(
            static_cast<int32_t>(size.x),
            static_cast<int32_t>(size.y),
            "Desktop Shadertoy", nullptr, nullptr);
    }
} // namespace

AppWindow::AppWindow(daxa::Device device, daxa_i32vec2 size)
    : glfw_window{create(size), &glfwDestroyWindow}, size{size} {
    layout.window_w = static_cast<float>(size.x);
    layout.window_h = static_cast<float>(size.y);

    glfwSetWindowUserPointer(this->glfw_window.get(), this);

    glfwSetWindowSizeCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, int width, int height) {
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            self.size = {width, height};
            self.layout.window_w = static_cast<float>(width);
            self.layout.window_h = static_cast<float>(height);
            self.swapchain.resize();
            if (self.on_resize) {
                self.on_resize();
            }
        });

    glfwSetWindowCloseCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window) {
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            if (self.on_close) {
                self.on_close();
            }
        });

    glfwSetKeyCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, int glfw_key, int scancode, int glfw_action, int glfw_mods) {
            ImGui_ImplGlfw_KeyCallback(glfw_window, glfw_key, scancode, glfw_action, glfw_mods);
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            bool forward = true;
            if (self.on_global_key) {
                forward = self.on_global_key(glfw_key, glfw_action, glfw_mods);
            }
            if (forward && self.should_forward_viewport_input() && self.on_key) {
                self.on_key(glfw_key, glfw_action);
            }
        });

    glfwSetCharCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, unsigned int codepoint) {
            ImGui_ImplGlfw_CharCallback(glfw_window, codepoint);
        });

    glfwSetCursorEnterCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, int entered) {
            ImGui_ImplGlfw_CursorEnterCallback(glfw_window, entered);
        });

    glfwSetCursorPosCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, double xpos, double ypos) {
            ImGui_ImplGlfw_CursorPosCallback(glfw_window, xpos, ypos);
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            if (self.should_forward_viewport_input() && self.on_mouse_move) {
                self.on_mouse_move(static_cast<float>(xpos), static_cast<float>(ypos));
            }
        });

    glfwSetMouseButtonCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, int button, int action, int mods) {
            ImGui_ImplGlfw_MouseButtonCallback(glfw_window, button, action, mods);
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            if (self.should_forward_viewport_input() && self.on_mouse_button) {
                self.on_mouse_button(button, action);
            }
        });

    glfwSetScrollCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, double xoffset, double yoffset) {
            ImGui_ImplGlfw_ScrollCallback(glfw_window, xoffset, yoffset);
        });

    glfwSetFramebufferSizeCallback(
        this->glfw_window.get(),
        [](GLFWwindow * /*glfw_window*/, int /*width*/, int /*height*/) {
        });

    glfwSetDropCallback(
        this->glfw_window.get(),
        [](GLFWwindow *glfw_window, int path_count, char const *paths[]) {
            auto &self = *reinterpret_cast<AppWindow *>(glfwGetWindowUserPointer(glfw_window));
            if (self.on_drop) {
                self.on_drop(std::span<char const *>{paths, static_cast<size_t>(path_count)});
            }
        });

    glfwSetWindowSizeLimits(this->glfw_window.get(), 760, 215 + 240, GLFW_DONT_CARE, GLFW_DONT_CARE);

    auto icon_image = GLFWimage{};
    icon_image.pixels = stbi_load((resource_dir / "appicon.png").string().c_str(), &icon_image.width, &icon_image.height, nullptr, 4);
    glfwSetWindowIcon(this->glfw_window.get(), 1, &icon_image);
    stbi_image_free(icon_image.pixels);

    this->swapchain = device.create_swapchain({
        .native_window = get_native_handle(this->glfw_window.get()),
        .native_window_platform = get_native_platform(this->glfw_window.get()),
        .surface_format_selector = [](daxa::Format format) -> int32_t {
            switch (format) {
            case daxa::Format::R8G8B8A8_UNORM: return 90;
            case daxa::Format::B8G8R8A8_UNORM: return 80;
            default: return 0;
            }
        },
        .present_mode = daxa::PresentMode::FIFO,
        .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::COLOR_ATTACHMENT,
        .max_allowed_frames_in_flight = 1,
        .name = "AppWindowSwapchain",
    });
}

void AppWindow::update() {
    glfwPollEvents();
}

void AppWindow::set_fullscreen(bool is_fullscreen) {
    layout_fullscreen = is_fullscreen;
    auto *monitor = glfwGetPrimaryMonitor();
    if (is_fullscreen) {
        GLFWvidmode const *mode = glfwGetVideoMode(monitor);
        glfwGetWindowPos(glfw_window.get(), &fullscreen_cache.pos.x, &fullscreen_cache.pos.y);
        fullscreen_cache.size = size;
        glfwSetWindowMonitor(glfw_window.get(), monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        size = {mode->width, mode->height};
    } else {
        glfwSetWindowMonitor(glfw_window.get(), nullptr, fullscreen_cache.pos.x, fullscreen_cache.pos.y, fullscreen_cache.size.x, fullscreen_cache.size.y, GLFW_DONT_CARE);
        size = fullscreen_cache.size;
    }
    sync_layout_from_window();
    swapchain.resize();
    if (on_resize) {
        on_resize();
    }
}

void AppWindow::set_vsync(bool enabled) {
    auto present_mode = enabled ? daxa::PresentMode::FIFO : daxa::PresentMode::IMMEDIATE;
    swapchain.set_present_mode(present_mode);
}

auto AppWindow::should_forward_viewport_input() const -> bool {
    if (layout_fullscreen) {
        return true;
    }
    auto const &io = ImGui::GetIO();
    if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
        return false;
    }
    return layout.point_in_viewport(io.MousePos.x, io.MousePos.y);
}

void AppWindow::sync_layout_from_window() {
    layout.window_w = static_cast<float>(size.x);
    layout.window_h = static_cast<float>(size.y);
}
