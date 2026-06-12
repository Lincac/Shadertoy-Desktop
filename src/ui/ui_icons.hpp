#pragma once

#include <filesystem>
#include <vector>

#include <daxa/daxa.hpp>
#include <daxa/utils/imgui.hpp>
#include <imgui.h>

struct UiIcon {
    ImTextureID tex_id{};
    ImVec2 size{16.f, 16.f};
    daxa::ImageId image{};
    daxa::SamplerId sampler{};
};

struct UiIcons {
    UiIcon reset{};
    UiIcon pause{};
    UiIcon play{};
    UiIcon save{};
    UiIcon download{};
    UiIcon settings{};
    UiIcon fullscreen{};
    UiIcon compile{};
    UiIcon open_file{};
    UiIcon font_size{};

    void load(std::filesystem::path const &icons_dir, daxa::Device device);
    /** Daxa ImGuiRenderer 每帧渲染后会清空除字体外的 texture 表，需每帧重建 ID */
    void refresh_texture_ids(daxa::ImGuiRenderer &renderer);
    void destroy(daxa::Device device);

    auto image_button(char const *id, UiIcon const &icon, ImVec2 size = ImVec2{22.f, 22.f},
                      ImVec4 tint = ImVec4{1.f, 1.f, 1.f, 1.f}) -> bool;

  private:
    UiIcon load_one(std::filesystem::path const &path, daxa::Device device);
    void register_icon(daxa::ImGuiRenderer &renderer, UiIcon &icon);
    std::vector<daxa::ImageId> images{};
    std::vector<daxa::SamplerId> samplers{};
};
