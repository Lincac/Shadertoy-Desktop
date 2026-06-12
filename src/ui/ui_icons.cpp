#include <ui/ui_icons.hpp>

#include <cstring>

#include <stb_image.h>

namespace {

    auto upload_rgba_texture(daxa::Device device, int width, int height, stbi_uc const *pixels) -> daxa::ImageId {
        auto const upload_size = static_cast<daxa::u32>(width * height * 4);
        auto image = device.create_image({
            .size = {static_cast<daxa::u32>(width), static_cast<daxa::u32>(height), 1},
            .usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "ui_icon",
        });

        auto staging = device.create_buffer({
            .size = upload_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "ui_icon_staging",
        });
        std::memcpy(device.buffer_host_address_as<daxa::u8>(staging).value(), pixels, upload_size);

        auto recorder = device.create_command_recorder({.name = "ui_icon_upload"});
        recorder.pipeline_barrier_image_transition({
            .dst_access = daxa::AccessConsts::TRANSFER_READ_WRITE,
            .dst_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .image_id = image,
        });
        recorder.copy_buffer_to_image({
            .buffer = staging,
            .image = image,
            .image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .image_extent = {static_cast<daxa::u32>(width), static_cast<daxa::u32>(height), 1},
        });
        recorder.pipeline_barrier_image_transition({
            .src_access = daxa::AccessConsts::TRANSFER_WRITE,
            .dst_access = daxa::AccessConsts::FRAGMENT_SHADER_READ,
            .src_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .dst_layout = daxa::ImageLayout::READ_ONLY_OPTIMAL,
            .image_id = image,
        });
        device.submit_commands({.command_lists = std::array{recorder.complete_current_commands()}});
        device.wait_idle();
        device.destroy_buffer(staging);
        return image;
    }

} // namespace

void UiIcons::register_icon(daxa::ImGuiRenderer &renderer, UiIcon &icon) {
    if (icon.image.is_empty()) {
        icon.tex_id = nullptr;
        return;
    }
    icon.tex_id = renderer.create_texture_id({.image_view_id = icon.image.default_view(), .sampler_id = icon.sampler});
}

auto UiIcons::load_one(std::filesystem::path const &path, daxa::Device device) -> UiIcon {
    UiIcon result{};
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return result;
    }

    result.image = upload_rgba_texture(device, w, h, pixels);
    stbi_image_free(pixels);
    result.sampler = device.create_sampler({.name = "ui_icon_sampler"});
    result.size = ImVec2{static_cast<float>(w), static_cast<float>(h)};
    images.push_back(result.image);
    samplers.push_back(result.sampler);
    return result;
}

void UiIcons::load(std::filesystem::path const &icons_dir, daxa::Device device) {
    reset = load_one(icons_dir / "rollback.png", device);
    pause = load_one(icons_dir / "pause.png", device);
    play = load_one(icons_dir / "play.png", device);
    save = load_one(icons_dir / "save-file.png", device);
    download = load_one(icons_dir / "find-location.png", device);
    settings = load_one(icons_dir / "setting.png", device);
    fullscreen = load_one(icons_dir / "resize.png", device);
    compile = load_one(icons_dir / "compile.png", device);
    open_file = load_one(icons_dir / "open-file.png", device);
    font_size = load_one(icons_dir / "font-size.png", device);
}

void UiIcons::refresh_texture_ids(daxa::ImGuiRenderer &renderer) {
    register_icon(renderer, reset);
    register_icon(renderer, pause);
    register_icon(renderer, play);
    register_icon(renderer, save);
    register_icon(renderer, download);
    register_icon(renderer, settings);
    register_icon(renderer, fullscreen);
    register_icon(renderer, compile);
    register_icon(renderer, open_file);
    register_icon(renderer, font_size);
}

void UiIcons::destroy(daxa::Device device) {
    for (auto sampler : samplers) {
        device.destroy_sampler(sampler);
    }
    for (auto image : images) {
        device.destroy_image(image);
    }
    samplers.clear();
    images.clear();
    reset = {};
    pause = {};
    play = {};
    save = {};
    download = {};
    settings = {};
    fullscreen = {};
    compile = {};
    open_file = {};
    font_size = {};
}

auto UiIcons::image_button(char const *id, UiIcon const &icon, ImVec2 size, ImVec4 tint) -> bool {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{0.f, 0.f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.f, 0.f, 0.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.f, 0.f, 0.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.f, 0.f, 0.f, 0.08f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.f, 0.f, 0.f, 0.15f});

    bool pressed = false;
    if (icon.tex_id == nullptr) {
        pressed = ImGui::Button(id, size);
    } else {
        pressed = ImGui::ImageButton(
            id,
            icon.tex_id,
            size,
            ImVec2{0.f, 0.f},
            ImVec2{1.f, 1.f},
            ImVec4{0.f, 0.f, 0.f, 0.f},
            tint);
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return pressed;
}
