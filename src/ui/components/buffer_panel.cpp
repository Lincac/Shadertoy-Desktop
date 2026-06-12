#include "buffer_panel.hpp"

#include <app/resources.hpp>
#include <ui/app_ui.hpp>
#include <ui/layout.hpp>
#include <ui/ui_theme.hpp>

#include <imgui.h>

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <fmt/format.h>

void replace_all(std::string &s, std::string const &toReplace, std::string const &replaceWith, bool wordBoundary = false);

namespace {

    auto default_glsl_code_for_pass_type(std::string const &pass_type) -> std::string {
        if (pass_type == "common") {
            return "vec4 someFunction( vec4 a, float b )\n{\n    return a+b;\n}";
        }
        if (pass_type == "cubemap") {
            return "void mainCubemap( out vec4 fragColor, in vec2 fragCoord, in vec3 rayOri, in vec3 rayDir )\n{\n"
                   "    vec3 col = 0.5 + 0.5*rayDir;\n"
                   "    fragColor = vec4(col,1.0);\n"
                   "}\n";
        }
        return "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n"
               "    vec2 uv = fragCoord/iResolution.xy;\n"
               "    vec3 col = 0.5 + 0.5*cos(iTime+uv.xyx+vec3(0,2,4));\n"
               "    fragColor = vec4(col,1.0);\n"
               "}\n";
    }

    auto resolve_pass_code(nlohmann::json &renderpass) -> std::string {
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

    auto find_pass(nlohmann::json &root, std::string const &pass_name) -> nlohmann::json * {
        auto &renderpasses = root["renderpass"];
        for (auto &renderpass : renderpasses) {
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

    struct BpiwOption {
        std::string label;
        std::string img_path;
        std::string resolution;
        std::string format_info;
    };

    struct BpiwPreviewGpu {
        daxa::ImageId image{};
        daxa::SamplerId sampler{};
        ImTextureID tex_id{};
    };

    auto bpiw_preview_cache() -> std::unordered_map<std::string, BpiwPreviewGpu> & {
        static auto cache = std::unordered_map<std::string, BpiwPreviewGpu>{};
        return cache;
    }

    auto upload_bpiw_preview(daxa::Device device, int width, int height, stbi_uc const *pixels) -> daxa::ImageId {
        auto const upload_size = static_cast<daxa::u32>(width * height * 4);
        auto image = device.create_image({
            .size = {static_cast<daxa::u32>(width), static_cast<daxa::u32>(height), 1},
            .usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "bpiw_preview",
        });
        auto staging = device.create_buffer({
            .size = upload_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "bpiw_preview_staging",
        });
        std::memcpy(device.buffer_host_address_as<daxa::u8>(staging).value(), pixels, upload_size);
        auto recorder = device.create_command_recorder({.name = "bpiw_preview_upload"});
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

    void ensure_bpiw_preview(std::string const &rel_path) {
        if (bpiw_preview_cache().contains(rel_path)) {
            return;
        }
        if (AppUi::s_instance == nullptr) {
            return;
        }
        auto const full_path = resource_dir / rel_path;
        if (!std::filesystem::exists(full_path)) {
            return;
        }
        int w = 0;
        int h = 0;
        int comp = 0;
        stbi_uc *pixels = stbi_load(full_path.string().c_str(), &w, &h, &comp, 4);
        if (pixels == nullptr || w <= 0 || h <= 0) {
            return;
        }
        BpiwPreviewGpu entry{};
        entry.image = upload_bpiw_preview(AppUi::s_instance->device, w, h, pixels);
        stbi_image_free(pixels);
        entry.sampler = AppUi::s_instance->device.create_sampler({.name = "bpiw_preview_sampler"});
        bpiw_preview_cache().emplace(rel_path, entry);
    }

    void refresh_bpiw_preview_textures() {
        if (AppUi::s_instance == nullptr || !AppUi::s_instance->imgui_renderer.has_value()) {
            return;
        }
        auto &renderer = *AppUi::s_instance->imgui_renderer;
        for (auto &[path, entry] : bpiw_preview_cache()) {
            if (entry.image.is_empty()) {
                entry.tex_id = nullptr;
                continue;
            }
            entry.tex_id = renderer.create_texture_id({.image_view_id = entry.image.default_view(), .sampler_id = entry.sampler});
            (void)path;
        }
    }

    void clear_bpiw_preview_cache(daxa::Device device) {
        for (auto &[path, entry] : bpiw_preview_cache()) {
            if (!entry.sampler.is_empty()) {
                device.destroy_sampler(entry.sampler);
            }
            if (!entry.image.is_empty()) {
                device.destroy_image(entry.image);
            }
            (void)path;
        }
        bpiw_preview_cache().clear();
    }

    auto bpiw_catalog() -> std::vector<BpiwOption> const & {
        static auto const catalog = std::vector<BpiwOption>{
            {"Keyboard", "media/icons/keyboard.png", "256 x 3", "1 ch, int8"},
            {"Buffer A", "media/icons/buffer00.png", "Viewport Res", "4 ch, Float32, linear"},
            {"Buffer B", "media/icons/buffer01.png", "Viewport Res", "4 ch, Float32, linear"},
            {"Buffer C", "media/icons/buffer02.png", "Viewport Res", "4 ch, Float32, linear"},
            {"Buffer D", "media/icons/buffer03.png", "Viewport Res", "4 ch, Float32, linear"},
            {"Cube A", "media/icons/cubemap00.png", "1024 x 1024", "4 ch, Float16, linear"},
            {"Abstract 1", "media/images/52d2a8f514c4fd2d9866587f4d7b2a5bfa1a11a0e772077d7682deb8b3b517e5.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Abstract 2", "media/images/bd6464771e47eed832c5eb2cd85cdc0bfc697786b903bfd30f890f9d4fc36657.jpg", "512 x 512", "3 ch, uint8"},
            {"Abstract 3", "media/images/8979352a182bde7c3c651ba2b2f4e0615de819585cc37b7175bcefbca15a6683.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Bayer", "media/images/85a6d68622b36995ccb98a89bbb119edf167c914660e4450d313de049320005c.png", "8 x 8", "1 ch, uint8"},
            {"Blue Noise", "media/images/cb49c003b454385aa9975733aff4571c62182ccdda480aaba9a8d250014f00ec.png", "1024 x 1024", "4 ch, uint8"},
            {"Font 1", "media/images/08b42b43ae9d3c0605da11d0eac86618ea888e62cdd9518ee8b9097488b31560.png", "1024 x 1024", "4 ch, uint8"},
            {"Gray Noise Medium", "media/images/0c7bf5fe9462d5bffbd11126e82908e39be3ce56220d900f633d58fb432e56f5.png", "256 x 256", "1 ch, uint8"},
            {"Gray Noise Small", "media/images/0a40562379b63dfb89227e6d172f39fdce9022cba76623f1054a2c83d6c0ba5d.png", "64 x 64", "1 ch, uint8"},
            {"Lichen", "media/images/fb918796edc3d2221218db0811e240e72e340350008338b0c07a52bd353666a6.jpg", "1024 x 1024", "3 ch, uint8"},
            {"London", "media/images/8de3a3924cb95bd0e95a443fff0326c869f9d4979cd1d5b6e94e2a01f5be53e9.jpg", "512 x 512", "3 ch, uint8"},
            {"Nyan Cat", "media/images/cbcbb5a6cfb55c36f8f021fbb0e3f69ac96339a39fa85cd96f2017a2192821b5.png", "256 x 32", "4 ch, uint8"},
            {"Organic 1", "media/images/cd4c518bc6ef165c39d4405b347b51ba40f8d7a065ab0e8d2e4f422cbc1e8a43.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Organic 2", "media/images/92d7758c402f0927011ca8d0a7e40251439fba3a1dac26f5b8b62026323501aa.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Organic 3", "media/images/79520a3d3a0f4d3caa440802ef4362e99d54e12b1392973e4ea321840970a88a.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Organic 4", "media/images/3871e838723dd6b166e490664eead8ec60aedd6b8d95bc8e2fe3f882f0fd90f0.jpg", "1024 x 1024", "3 ch, uint8"},
            {"Pebbles", "media/images/ad56fba948dfba9ae698198c109e71f118a54d209c0ea50d77ea546abad89c57.png", "512 x 512", "1 ch, uint8"},
            {"RGBA Noise Medium", "media/images/f735bee5b64ef98879dc618b016ecf7939a5756040c2cde21ccb15e69a6e1cfb.png", "256 x 256", "4 ch, uint8"},
            {"RGBA Noise Small", "media/images/3083c722c0c738cad0f468383167a0d246f91af2bfa373e9c5c094fb8c8413e0.png", "64 x 64", "4 ch, uint8"},
            {"Rock Tiles", "media/images/10eb4fe0ac8a7dc348a2cc282ca5df1759ab8bf680117e4047728100969e7b43.jpg", "512 x 512", "3 ch, uint8"},
            {"Rusty Metal", "media/images/95b90082f799f48677b4f206d856ad572f1d178c676269eac6347631d4447258.jpg", "512 x 512", "3 ch, uint8"},
            {"Stars", "media/images/e6e5631ce1237ae4c05b3563eda686400a401df4548d0f9fad40ecac1659c46c.jpg", "512 x 512", "3 ch, uint8"},
            {"Wood", "media/images/1f7dca9c22f324751f2a5a59c9b181dfe3b5564a04b724c657732d0bf09c99db.jpg", "1024 x 1024", "1 ch, uint8"},
            {"Forest", "media/images/94284d43be78f00eb6b298e6d78656a1b34e2b91b34940d02f1ca8b22310e8a0.png", "256 x 256", "3 ch, uint8"},
            {"Forest Blurred", "media/images/0681c014f6c88c356cf9c0394ffe015acc94ec1474924855f45d22c3e70b5785.png", "64 x 64", "3 ch, uint8"},
            {"St. Peter's Basilica", "media/images/488bd40303a2e2b9a71987e48c66ef41f5e937174bf316d3ed0e86410784b919.jpg", "256 x 256", "3 ch, uint8"},
            {"St. Peter's Blurred", "media/images/550a8cce1bf403869fde66dddf6028dd171f1852f4a704a465e1b80d23955663.png", "64 x 64", "3 ch, uint8"},
            {"Uffizi Gallery", "media/images/585f9546c092f53ded45332b343144396c0b2d70d9965f585ebc172080d8aa58.jpg", "256 x 256", "3 ch, uint8"},
            {"Uffizi Gallery Blurred", "media/images/793a105653fbdadabdc1325ca08675e1ce48ae5f12e37973829c87bea4be3232.png", "64 x 64", "3 ch, uint8"},
            {"Gray Noise3D", "media/icons/volume_gray.png", "32 x 32 x 32", "1 ch, uint8"},
            {"RGBA Noise3D", "media/icons/volume_rgba.png", "32 x 32 x 32", "4 ch, uint8"},
        };
        return catalog;
    }

    auto cubemap_image_paths() -> std::array<std::string_view, 6> const & {
        static auto const paths = std::array<std::string_view, 6>{
            "media/images/94284d43be78f00eb6b298e6d78656a1b34e2b91b34940d02f1ca8b22310e8a0.png",
            "media/images/0681c014f6c88c356cf9c0394ffe015acc94ec1474924855f45d22c3e70b5785.png",
            "media/images/488bd40303a2e2b9a71987e48c66ef41f5e937174bf316d3ed0e86410784b919.jpg",
            "media/images/550a8cce1bf403869fde66dddf6028dd171f1852f4a704a465e1b80d23955663.png",
            "media/images/585f9546c092f53ded45332b343144396c0b2d70d9965f585ebc172080d8aa58.jpg",
            "media/images/793a105653fbdadabdc1325ca08675e1ce48ae5f12e37973829c87bea4be3232.png",
        };
        return paths;
    }

    auto is_cubemap_image(std::string const &path) -> bool {
        for (auto const &cubemap_path : cubemap_image_paths()) {
            if (path == cubemap_path) {
                return true;
            }
        }
        return false;
    }

    auto misc_bpiw_options() -> std::vector<BpiwOption> {
        auto result = std::vector<BpiwOption>{};
        for (auto const &opt : bpiw_catalog()) {
            if (opt.img_path.starts_with("media/icons/") && opt.label != "Gray Noise3D" && opt.label != "RGBA Noise3D") {
                if (std::filesystem::exists(resource_dir / opt.img_path)) {
                    result.push_back(opt);
                }
            }
        }
        return result;
    }

    auto texture_bpiw_options() -> std::vector<BpiwOption> {
        auto result = std::vector<BpiwOption>{};
        for (auto const &opt : bpiw_catalog()) {
            if (opt.img_path.starts_with("media/images/") && !is_cubemap_image(opt.img_path) &&
                std::filesystem::exists(resource_dir / opt.img_path)) {
                result.push_back(opt);
            }
        }
        return result;
    }

    auto cubemap_bpiw_options() -> std::vector<BpiwOption> {
        static auto const names = std::array{
            "Forest",
            "Forest Blurred",
            "St. Peter's Basilica",
            "St. Peter's Blurred",
            "Uffizi Gallery",
            "Uffizi Gallery Blurred",
        };
        auto result = std::vector<BpiwOption>{};
        for (auto const *name : names) {
            for (auto const &opt : bpiw_catalog()) {
                if (opt.label == name && std::filesystem::exists(resource_dir / opt.img_path)) {
                    result.push_back(opt);
                    break;
                }
            }
        }
        return result;
    }

    auto volume_bpiw_options() -> std::vector<BpiwOption> {
        auto result = std::vector<BpiwOption>{};
        for (auto const &opt : bpiw_catalog()) {
            if (opt.label == "Gray Noise3D" || opt.label == "RGBA Noise3D") {
                if (std::filesystem::exists(resource_dir / opt.img_path)) {
                    result.push_back(opt);
                }
            }
        }
        return result;
    }

    void draw_bpiw_orange_separator() {
        auto const line_y = ImGui::GetCursorScreenPos().y;
        auto const line_x0 = ImGui::GetWindowPos().x;
        auto const line_x1 = line_x0 + ImGui::GetWindowWidth();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2{line_x0, line_y},
            ImVec2{line_x1, line_y + ui_layout::k_tab_orange_border},
            IM_COL32(248, 176, 48, 255));
        ImGui::Dummy(ImVec2{0.f, ui_layout::k_tab_orange_border});
    }

    auto draw_bpiw_option_card(BpiwOption const &opt) -> bool {
        ensure_bpiw_preview(opt.img_path);
        auto const card_size = ImVec2{ui_layout::k_bpiw_card_w, ui_layout::k_bpiw_card_h};
        auto const pos = ImGui::GetCursorScreenPos();
        auto *draw = ImGui::GetWindowDrawList();
        ImGui::PushID(opt.img_path.c_str());
        ImGui::InvisibleButton("##bpiw_card", card_size);
        bool const hovered = ImGui::IsItemHovered();
        bool const pressed = ImGui::IsItemClicked();
        ImGui::PopID();

        auto const card_max = ImVec2{pos.x + card_size.x, pos.y + card_size.y};
        auto const bg = IM_COL32(190, 190, 190, 255);
        auto const border = hovered ? IM_COL32(0, 0, 0, 255) : bg;
        draw->AddRectFilled(pos, card_max, bg);
        draw->AddRect(pos, card_max, border);

        auto const preview_min = ImVec2{pos.x + 4.f, pos.y + 4.f};
        auto const preview_max = ImVec2{preview_min.x + ui_layout::k_bpiw_preview_size, preview_min.y + ui_layout::k_bpiw_preview_size};
        draw->AddRectFilled(preview_min, preview_max, IM_COL32(0, 0, 0, 255));

        auto preview_it = bpiw_preview_cache().find(opt.img_path);
        if (preview_it != bpiw_preview_cache().end() && preview_it->second.tex_id != nullptr) {
            draw->AddImage(preview_it->second.tex_id, preview_min, preview_max);
        }

        auto const text_x = preview_max.x + 4.f;
        auto const text_col = IM_COL32(0, 0, 0, 255);
        auto *font = ImGui::GetFont();
        float const font_size = ImGui::GetFontSize() * 0.85f;
        draw->AddText(font, font_size, ImVec2{text_x, pos.y + 4.f}, text_col, opt.label.c_str());
        draw->AddText(font, font_size, ImVec2{text_x, pos.y + 18.f}, text_col, "by shadertoy");
        draw->AddText(font, font_size, ImVec2{text_x, pos.y + 44.f}, text_col, opt.resolution.c_str());
        draw->AddText(font, font_size, ImVec2{text_x, pos.y + 58.f}, text_col, opt.format_info.c_str());

        return pressed;
    }

    void draw_bpiw_option_grid(std::vector<BpiwOption> const &options, int tab_id, std::function<void(BpiwOption const &)> const &on_select) {
        ImGui::PushID(tab_id);
        ImGui::BeginChild("BpiwScroll", ImVec2{-1.f, 0.f}, false);
        int col = 0;
        for (auto const &opt : options) {
            if (col > 0) {
                ImGui::SameLine(0.f, ui_layout::k_bpiw_col_gap);
            }
            if (draw_bpiw_option_card(opt)) {
                on_select(opt);
            }
            ++col;
            if (col >= ui_layout::k_bpiw_cols) {
                col = 0;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    auto channel_preview_path(nlohmann::json const &input, nlohmann::json const &renderpasses) -> std::string {
        auto path = std::string{};
        if (input.contains("filepath")) {
            path = input["filepath"].get<std::string>();
        } else if (input.contains("src")) {
            path = input["src"].get<std::string>();
        }
        replace_all(path, "/media/a/", "media/images/");

        auto input_type = std::string{};
        if (input.contains("type")) {
            input_type = input["type"].get<std::string>();
        } else if (input.contains("ctype")) {
            input_type = input["ctype"].get<std::string>();
        }

        if (std::filesystem::exists(resource_dir / path)) {
            return path;
        }
        if (input_type == "keyboard") {
            return "media/icons/keyboard.png";
        }
        if (input_type == "buffer") {
            auto buffer_index = 0;
            for (auto &rp : renderpasses) {
                if (rp["type"] == "buffer") {
                    if (rp["outputs"][0]["id"] == input["id"]) {
                        break;
                    }
                    ++buffer_index;
                }
            }
            return fmt::format("media/icons/buffer0{}.png", buffer_index);
        }
        if (input_type == "cubemap") {
            return "media/icons/cubemap00.png";
        }
        if (input_type == "volume") {
            return "media/icons/volume_gray.png";
        }
        return path;
    }

    void draw_channel_slot_preview(ImVec2 slot_min, ImVec2 slot_max, std::string const &rel_path) {
        if (rel_path.empty()) {
            return;
        }
        ensure_bpiw_preview(rel_path);
        auto preview_it = bpiw_preview_cache().find(rel_path);
        if (preview_it != bpiw_preview_cache().end() && preview_it->second.tex_id != nullptr) {
            ImGui::GetWindowDrawList()->AddImage(preview_it->second.tex_id, slot_min, slot_max);
        }
    }

} // namespace

namespace {
    auto constexpr k_pass_tab_order = std::array{
        "Common",
        "Buffer A",
        "Buffer B",
        "Buffer C",
        "Buffer D",
        "Cube A",
        "Image",
    };
} // namespace

void BufferPanel::shutdown_gpu_resources(daxa::Device device) {
    clear_bpiw_preview_cache(device);
}

void BufferPanel::sort_renderpasses() {
    if (!json.contains("renderpass") || !json["renderpass"].is_array()) {
        return;
    }
    auto &renderpasses = json["renderpass"];
    auto sorted = nlohmann::json::array();
    for (auto const *name : k_pass_tab_order) {
        for (auto &renderpass : renderpasses) {
            if (renderpass.contains("name") && renderpass["name"] == name) {
                sorted.push_back(renderpass);
                break;
            }
        }
    }
    for (auto &renderpass : renderpasses) {
        if (!renderpass.contains("name")) {
            continue;
        }
        auto const &name = renderpass["name"].get<std::string>();
        bool const known = std::any_of(k_pass_tab_order.begin(), k_pass_tab_order.end(), [&](char const *slot) {
            return name == slot;
        });
        if (!known) {
            sorted.push_back(renderpass);
        }
    }
    renderpasses = std::move(sorted);
}

auto BufferPanel::pass_count() const -> int {
    if (!json.contains("renderpass") || !json["renderpass"].is_array()) {
        return 0;
    }
    return static_cast<int>(json["renderpass"].size());
}

auto BufferPanel::pass_name_at(int tab_index) const -> std::string {
    if (tab_index < 0 || tab_index >= pass_count()) {
        return {};
    }
    return json["renderpass"][tab_index]["name"].get<std::string>();
}

auto BufferPanel::active_pass_name() const -> std::string {
    return pass_name_at(active_tab);
}

void BufferPanel::rebuild_code_buffers_from_json() {
    pass_code_buffers.clear();
    if (!json.contains("renderpass")) {
        return;
    }
    for (auto &renderpass : json["renderpass"]) {
        auto name = renderpass["name"].get<std::string>();
        auto code = resolve_pass_code(renderpass);
        replace_all(code, "\\n", "\n");
        auto &buf = pass_code_buffers[name];
        buf.assign(pass_code_max_chars + 1, '\0');
        if (code.size() >= pass_code_max_chars) {
            code.resize(pass_code_max_chars);
        }
        std::memcpy(buf.data(), code.data(), code.size());
    }
}

void BufferPanel::sync_pass_code_to_json(std::string const &pass_name) {
    auto it = pass_code_buffers.find(pass_name);
    if (it == pass_code_buffers.end()) {
        return;
    }
    auto *pass_ptr = find_pass(json, pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto code = std::string{it->second.data()};
    if ((*pass_ptr)["code"].get<std::string>() != code) {
        (*pass_ptr)["code"] = code;
    }
}

void BufferPanel::sync_active_pass_code_to_json() {
    sync_pass_code_to_json(active_pass_name());
}

void BufferPanel::sync_all_pass_codes_to_json() {
    for (auto const &[name, _] : pass_code_buffers) {
        sync_pass_code_to_json(name);
    }
}

void BufferPanel::load_shadertoy_json(nlohmann::json const &temp_json) {
    if (temp_json.contains("numShaders")) {
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
    reload_json();
}

void BufferPanel::reload_json() {
    sort_renderpasses();
    rebuild_code_buffers_from_json();
    for (int i = 0; i < pass_count(); ++i) {
        if (pass_name_at(i) == "Image") {
            active_tab = i;
            break;
        }
    }
    if (active_tab >= pass_count()) {
        active_tab = std::max(0, pass_count() - 1);
    }
    channel_show_sampler = {};
    dirty = true;
}

void BufferPanel::compile_shader() {
    sync_active_pass_code_to_json();
    sync_all_pass_codes_to_json();
    dirty = true;
    if (on_recompile) {
        on_recompile();
    }
}

void BufferPanel::load_json_from_dialog() {
    if (AppUi::s_instance != nullptr) {
        AppUi::s_instance->load_json();
    }
}

void BufferPanel::add_pass(std::string const &pass_kind) {
    if (find_pass(json, pass_kind) != nullptr) {
        return;
    }

    nlohmann::json new_pass;
    if (pass_kind == "Common") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [], "inputs": [],
            "code": "vec4 someFunction( vec4 a, float b )\n{\n    return a+b;\n}",
            "name": "Common", "description": "", "type": "common"
        })");
    } else if (pass_kind == "Buffer A") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [{"channel": 0, "id": "4dXGR8"}], "inputs": [],
            "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
            "name": "Buffer A", "description": "", "type": "buffer"
        })");
    } else if (pass_kind == "Buffer B") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [{"channel": 0, "id": "XsXGR8"}], "inputs": [],
            "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
            "name": "Buffer B", "description": "", "type": "buffer"
        })");
    } else if (pass_kind == "Buffer C") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [{"channel": 0, "id": "4sXGR8"}], "inputs": [],
            "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
            "name": "Buffer C", "description": "", "type": "buffer"
        })");
    } else if (pass_kind == "Buffer D") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [{"channel": 0, "id": "XdfGR8"}], "inputs": [],
            "code": "void mainImage( out vec4 fragColor, in vec2 fragCoord )\n{\n    fragColor = vec4(0.0,0.0,1.0,1.0);\n}",
            "name": "Buffer D", "description": "", "type": "buffer"
        })");
    } else if (pass_kind == "Cube A") {
        new_pass = nlohmann::json::parse(R"({
            "outputs": [{"channel": 0, "id": "4dX3Rr"}], "inputs": [],
            "code": "void mainCubemap( out vec4 fragColor, in vec2 fragCoord, in vec3 rayOri, in vec3 rayDir )\n{\n    vec3 col = 0.5 + 0.5*rayDir;\n    fragColor = vec4(col,1.0);\n}",
            "name": "Cube A", "description": "", "type": "cubemap"
        })");
    } else {
        return;
    }

    json["renderpass"].push_back(new_pass);
    reload_json();
    for (int i = 0; i < pass_count(); ++i) {
        if (pass_name_at(i) == pass_kind) {
            active_tab = i;
            break;
        }
    }
}

void BufferPanel::close_tab(int tab_index) {
    auto const name = pass_name_at(tab_index);
    if (name == "Image") {
        return;
    }
    auto const active_name = active_pass_name();
    auto &renderpasses = json["renderpass"];
    for (auto iter = renderpasses.begin(); iter != renderpasses.end(); ++iter) {
        if ((*iter)["name"] == name) {
            renderpasses.erase(iter);
            break;
        }
    }
    reload_json();
    if (name == active_name) {
        active_tab = std::max(0, tab_index - 1);
        if (active_tab >= pass_count()) {
            active_tab = std::max(0, pass_count() - 1);
        }
        return;
    }
    for (int i = 0; i < pass_count(); ++i) {
        if (pass_name_at(i) == active_name) {
            active_tab = i;
            return;
        }
    }
}

void BufferPanel::remove_channel_input(std::string const &pass_name, int channel_index) {
    auto *pass_ptr = find_pass(json, pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;
    int current_index = 0;
    for (auto it = pass["inputs"].begin(); it != pass["inputs"].end(); ++it, ++current_index) {
        if ((*it)["channel"] == channel_index) {
            pass["inputs"].erase(it);
            dirty = true;
            break;
        }
    }
}

void BufferPanel::apply_bpiw_selection(std::string const &img_path, int bpiw_category) {
    if (bpiw_pass_name.empty()) {
        return;
    }
    auto *pass_ptr = find_pass(json, bpiw_pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;

    auto img_path_normalized = img_path;
    replace_all(img_path_normalized, "\\", "/");

    auto default_sampler = nlohmann::json{};
    default_sampler["filter"] = "linear";
    default_sampler["wrap"] = "clamp";
    default_sampler["vflip"] = "true";
    default_sampler["srgb"] = "false";
    default_sampler["internal"] = "byte";

    auto new_input = nlohmann::json{};
    new_input["id"] = std::to_string(std::hash<std::string>{}(img_path_normalized));
    new_input["channel"] = bpiw_target_channel;
    new_input["sampler"] = default_sampler;
    new_input["published"] = 1;

    auto const rel_icon = [&](std::string const &p) { return p; };

    switch (bpiw_category) {
    case 0: {
        if (img_path_normalized == rel_icon("media/icons/keyboard.png")) {
            new_input["filepath"] = "/presets/tex00.jpg";
            new_input["type"] = "keyboard";
            new_input["sampler"]["filter"] = "nearest";
        } else if (img_path_normalized == rel_icon("media/icons/buffer00.png")) {
            new_input["id"] = "4dXGR8";
            new_input["filepath"] = "/media/previz/buffer00.png";
            new_input["type"] = "buffer";
        } else if (img_path_normalized == rel_icon("media/icons/buffer01.png")) {
            new_input["id"] = "XsXGR8";
            new_input["filepath"] = "/media/previz/buffer01.png";
            new_input["type"] = "buffer";
        } else if (img_path_normalized == rel_icon("media/icons/buffer02.png")) {
            new_input["id"] = "4sXGR8";
            new_input["filepath"] = "/media/previz/buffer02.png";
            new_input["type"] = "buffer";
        } else if (img_path_normalized == rel_icon("media/icons/buffer03.png")) {
            new_input["id"] = "XdfGR8";
            new_input["filepath"] = "/media/previz/buffer03.png";
            new_input["type"] = "buffer";
        } else if (img_path_normalized == rel_icon("media/icons/cubemap00.png")) {
            new_input["id"] = "4dX3Rr";
            new_input["filepath"] = "/media/previz/cubemap00.png";
            new_input["type"] = "cubemap";
        }
    } break;
    case 1: {
        auto fp = img_path_normalized;
        if (auto pos = fp.find("media/images/"); pos != std::string::npos) {
            fp = "/media/a/" + fp.substr(pos + std::string{"media/images/"}.size());
        } else if (!fp.starts_with("/")) {
            fp = "/" + fp;
        }
        new_input["filepath"] = fp;
        new_input["type"] = "texture";
        new_input["sampler"]["filter"] = "mipmap";
        new_input["sampler"]["wrap"] = "repeat";
    } break;
    case 2: {
        auto fp = img_path_normalized;
        if (auto pos = fp.find("media/images/"); pos != std::string::npos) {
            fp = "/media/a/" + fp.substr(pos + std::string{"media/images/"}.size());
        } else if (!fp.starts_with("/")) {
            fp = "/" + fp;
        }
        new_input["filepath"] = fp;
        new_input["type"] = "cubemap";
    } break;
    case 3: {
        if (img_path_normalized == rel_icon("media/icons/volume_gray.png")) {
            new_input["id"] = "4sfGRr";
            new_input["filepath"] = "/media/a/27012b4eadd0c3ce12498b867058e4f717ce79e10a99568cca461682d84a4b04.bin";
            new_input["type"] = "volume";
        } else if (img_path_normalized == rel_icon("media/icons/volume_rgba.png")) {
            new_input["id"] = "XdX3Rr";
            new_input["filepath"] = "/media/a/aea6b99da1d53055107966b59ac5444fc8bc7b3ce2d0bbb6a4a3cbae1d97f3aa.bin";
            new_input["type"] = "volume";
        } else if (img_path_normalized == rel_icon("media/icons/buffer00.png")) {
            new_input["id"] = "XdX3Rr";
            new_input["filepath"] = "/media/a/aea6b99da1d53055107966b59ac5444fc8bc7b3ce2d0bbb6a4a3cbae1d97f3aa.bin";
            new_input["type"] = "volume";
        }
    } break;
    default: break;
    }

    auto *channel_input = find_input(pass, bpiw_target_channel);
    if (channel_input != nullptr) {
        *channel_input = new_input;
    } else {
        pass["inputs"].push_back(new_input);
    }

    show_bpiw = false;
    dirty = true;
}

void BufferPanel::draw_tabs_and_add() {
    auto const count = pass_count();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{6.f, 3.f});
    ImGui::SetCursorPosX(4.f);
    if (ImGui::Button("+", ImVec2{22.f, 20.f})) {
        ImGui::OpenPopup("AddPassPopup");
    }
    ImVec2 const add_btn_min = ImGui::GetItemRectMin();
    ImVec2 const add_btn_max = ImGui::GetItemRectMax();
    if (ImGui::IsPopupOpen("AddPassPopup", ImGuiPopupFlags_None)) {
        ImGui::SetNextWindowPos(ImVec2{add_btn_min.x, add_btn_max.y}, ImGuiCond_Appearing);
    }
    if (ImGui::BeginPopup("AddPassPopup")) {
        auto try_add = [&](char const *label, char const *kind) {
            if (find_pass(json, kind) == nullptr && ImGui::Selectable(label)) {
                add_pass(kind);
                ImGui::CloseCurrentPopup();
            }
        };
        try_add("Common", "Common");
        try_add("Buffer A", "Buffer A");
        try_add("Buffer B", "Buffer B");
        try_add("Buffer C", "Buffer C");
        try_add("Buffer D", "Buffer D");
        try_add("Cube A", "Cube A");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(30.f);
    if (ImGui::BeginTabBar("PassTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < count; ++i) {
            auto name = pass_name_at(i);
            bool open = true;
            bool const is_image = (name == "Image");
            if (ImGui::BeginTabItem(name.c_str(), is_image ? nullptr : &open)) {
                if (active_tab != i) {
                    sync_active_pass_code_to_json();
                    active_tab = i;
                    channel_show_sampler = {};
                }
                ImGui::EndTabItem();
            }
            if (!is_image && !open) {
                sync_active_pass_code_to_json();
                close_tab(i);
                break;
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();

    auto const line_y = ImGui::GetCursorScreenPos().y;
    auto const line_x0 = ImGui::GetWindowPos().x;
    auto const line_x1 = line_x0 + ImGui::GetWindowWidth();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2{line_x0, line_y},
        ImVec2{line_x1, line_y + ui_layout::k_tab_orange_border},
        IM_COL32(248, 176, 48, 255));
    ImGui::Dummy(ImVec2{0.f, ui_layout::k_tab_orange_border});
}

void BufferPanel::draw_shader_input_section() {
    ImGui::PushStyleColor(ImGuiCol_Header, ui_theme::k_shader_input_header);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.80f, 0.80f, 0.80f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{0.72f, 0.72f, 0.72f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Border, ui_theme::k_editor_border);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
    if (show_shader_input) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (ImGui::CollapsingHeader("Shader Input", flags)) {
        show_shader_input = true;

        bool const use_code_font = AppUi::s_instance != nullptr && AppUi::s_instance->code_font != nullptr;
        if (use_code_font) {
            ImGui::PushFont(AppUi::s_instance->code_font);
        }

        static constexpr int k_uniform_line_count = 11;
        ImVec2 const body_pad{8.f, 6.f};
        float const line_h = ImGui::GetTextLineHeightWithSpacing();
        float const box_h = line_h * static_cast<float>(k_uniform_line_count) + body_pad.y * 2.f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_theme::k_shader_input_body);
        ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::k_editor_text);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, body_pad);
        ImGui::BeginChild("ShaderInputBody", ImVec2{-1.f, box_h}, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextUnformatted("uniform vec3  iResolution;   // viewport resolution (in pixels)");
        ImGui::TextUnformatted("uniform float iTime;         // shader playback time (in seconds)");
        ImGui::TextUnformatted("uniform float iTimeDelta;    // render time (in seconds)");
        ImGui::TextUnformatted("uniform float iFrameRate;    // shader frame rate");
        ImGui::TextUnformatted("uniform int   iFrame;        // shader playback frame");
        ImGui::TextUnformatted("uniform float iChannelTime[4];");
        ImGui::TextUnformatted("uniform vec3  iChannelResolution[4];");
        ImGui::TextUnformatted("uniform vec4  iMouse;");
        ImGui::TextUnformatted("uniform samplerXX iChannel0..3;");
        ImGui::TextUnformatted("uniform vec4  iDate;");
        ImGui::TextUnformatted("uniform float iSampleRate;");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (use_code_font) {
            ImGui::PopFont();
        }
    } else {
        show_shader_input = false;
    }
    ImGui::PopStyleColor(4);
}

auto BufferPanel::active_pass_is_common() const -> bool {
    if (active_tab < 0 || active_tab >= pass_count()) {
        return false;
    }
    auto const &renderpass = json["renderpass"][active_tab];
    return renderpass.contains("type") && renderpass["type"] == "common";
}

void BufferPanel::draw_code_editor(float editor_height) {
    auto const pass_name = active_pass_name();
    if (pass_name.empty()) {
        return;
    }
    auto it = pass_code_buffers.find(pass_name);
    if (it == pass_code_buffers.end()) {
        return;
    }
    auto &buf = it->second;
    if (buf.size() < pass_code_max_chars + 1) {
        buf.resize(pass_code_max_chars + 1, '\0');
    }

    int const editor_style = ui_theme::push_code_editor_style();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4.f, 4.f});
    ImGui::BeginChild("CodeEditor", ImVec2{-1.f, editor_height}, true);
    ImGui::PushID(pass_name.c_str());
    if (AppUi::s_instance != nullptr && AppUi::s_instance->code_font != nullptr) {
        ImGui::PushFont(AppUi::s_instance->code_font);
    }
    ImGuiInputTextFlags const flags = ImGuiInputTextFlags_AllowTabInput;
    ImGui::InputTextMultiline("##code", buf.data(), buf.size(), ImVec2{-1.f, -1.f}, flags);
    if (AppUi::s_instance != nullptr && AppUi::s_instance->code_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ui_theme::pop_code_editor_style(editor_style);
}

void BufferPanel::draw_code_toolbar() {
    auto const pass_name = active_pass_name();
    if (pass_name.empty()) {
        return;
    }
    auto it = pass_code_buffers.find(pass_name);
    if (it == pass_code_buffers.end()) {
        return;
    }
    auto &buf = it->second;

    auto const width = ImGui::GetContentRegionAvail().x;
    auto const height = ui_layout::k_code_toolbar_height;
    auto const pos = ImGui::GetCursorScreenPos();
    auto const rounding = ui_layout::k_code_toolbar_corner_radius;
    auto const rect_max = ImVec2{pos.x + width, pos.y + height};
    auto *draw = ImGui::GetWindowDrawList();
    auto const bg = ImGui::ColorConvertFloat4ToU32(ui_theme::k_toolbar_bg);
    auto const border = ImGui::ColorConvertFloat4ToU32(ui_theme::k_editor_border);
    draw->AddRectFilled(pos, rect_max, bg, rounding);
    draw->AddRect(pos, rect_max, border, rounding);

    auto const used = std::strlen(buf.data());
    auto const row_y = pos.y + (height - ui_layout::k_bottom_bar_icon_size) * 0.5f;
    auto const icon_size = ImVec2{ui_layout::k_bottom_bar_icon_size, ui_layout::k_bottom_bar_icon_size};
    auto const icon_gap = 4.f;
    auto const right_pad = 4.f;

    ImGui::SetCursorScreenPos(ImVec2{pos.x + 4.f, row_y});
    if (AppUi::s_instance != nullptr && AppUi::s_instance->icons.compile.tex_id != nullptr) {
        if (AppUi::s_instance->icons.image_button("##compile", AppUi::s_instance->icons.compile, icon_size)) {
            compile_shader();
        }
    } else if (ImGui::Button("\xE2\x96\xB6", icon_size)) {
        compile_shader();
    }

    auto const stats = fmt::format("{}/{}", used, pass_code_max_chars);
    auto const stats_w = ImGui::CalcTextSize(stats.c_str()).x;
    ImGui::SetCursorScreenPos(ImVec2{pos.x + (width - stats_w) * 0.5f, row_y + 2.f});
    ImGui::TextUnformatted(stats.c_str());

    auto const right_icon_x = pos.x + width - right_pad - icon_size.x;
    ImGui::SetCursorScreenPos(ImVec2{right_icon_x, row_y});
    if (AppUi::s_instance != nullptr && AppUi::s_instance->icons.font_size.tex_id != nullptr) {
        if (AppUi::s_instance->icons.image_button("##fontsize", AppUi::s_instance->icons.font_size, icon_size)) {
            ImGui::OpenPopup("CodeFontSizePopup");
        }
        auto const btn_max = ImGui::GetItemRectMax();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{8.f, 4.f});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.f);
        ImGui::SetNextWindowPos(ImVec2{btn_max.x, pos.y}, ImGuiCond_Appearing, ImVec2{1.f, 1.f});
        if (ImGui::BeginPopup("CodeFontSizePopup")) {
            for (size_t i = 0; i < AppUi::k_code_font_sizes.size(); ++i) {
                auto const label = fmt::format("{:.0f}", AppUi::k_code_font_sizes[i]);
                bool const selected = static_cast<int>(i) == AppUi::s_instance->code_font_size_index;
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    AppUi::s_instance->set_code_font_size(static_cast<int>(i));
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }

    ImGui::SetCursorScreenPos(ImVec2{right_icon_x - icon_gap - icon_size.x, row_y});
    if (AppUi::s_instance != nullptr && AppUi::s_instance->icons.open_file.tex_id != nullptr) {
        if (AppUi::s_instance->icons.image_button("##openfile", AppUi::s_instance->icons.open_file, icon_size)) {
            load_json_from_dialog();
        }
    } else if (ImGui::Button("...", icon_size)) {
        load_json_from_dialog();
    }

    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2{width, height});
}

void BufferPanel::draw_channels() {
    auto const pass_name = active_pass_name();
    if (pass_name.empty()) {
        return;
    }
    auto *pass_ptr = find_pass(json, pass_name);
    if (pass_ptr == nullptr) {
        return;
    }
    auto &pass = *pass_ptr;
    if (pass.contains("type") && pass["type"] == "common") {
        return;
    }

    auto &renderpasses = json["renderpass"];
    auto const slot_w = ui_layout::k_channel_col_width;
    auto const slot_h = ui_layout::k_ichannel_slot_height;
    auto const label_h = ui_layout::k_ichannel_label_height;

    refresh_bpiw_preview_textures();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.f, 0.f});
    ImGui::BeginGroup();
    for (int ch = 0; ch < 4; ++ch) {
        if (ch > 0) {
            ImGui::SameLine(0.f, ui_layout::k_channel_col_spacer);
        }
        ImGui::BeginGroup();
        ImGui::PushID(ch);

        auto *input = find_input(pass, ch);
        bool const has_input = input != nullptr;
        auto const close_size = ImVec2{18.f, 18.f};

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.f, 0.f, 0.f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.15f, 0.15f, 0.15f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.25f, 0.25f, 0.25f, 1.f});
        if (ImGui::Button("##slot", ImVec2{slot_w, slot_h})) {
            auto const click_min = ImGui::GetItemRectMin();
            auto const close_min = ImVec2{click_min.x + slot_w - close_size.x - 8.f, click_min.y};
            auto const close_max = ImVec2{close_min.x + close_size.x, close_min.y + close_size.y};
            if (has_input && ImGui::IsMouseHoveringRect(close_min, close_max)) {
                remove_channel_input(pass_name, ch);
                channel_show_sampler[ch] = false;
            } else {
                show_bpiw = true;
                bpiw_pass_name = pass_name;
                bpiw_target_channel = ch;
            }
        }
        ImGui::PopStyleColor(3);

        auto const slot_min = ImGui::GetItemRectMin();
        auto const slot_max = ImGui::GetItemRectMax();
        auto const label_y = slot_max.y;
        auto *draw = ImGui::GetWindowDrawList();

        if (has_input) {
            if (!channel_show_sampler[ch]) {
                auto const preview_path = channel_preview_path(*input, renderpasses);
                draw_channel_slot_preview(slot_min, slot_max, preview_path);
            }

            auto const close_min = ImVec2{slot_min.x + slot_w - close_size.x - 8.f, slot_min.y};
            auto const close_max = ImVec2{close_min.x + close_size.x, close_min.y + close_size.y};
            draw->AddRectFilled(close_min, close_max, IM_COL32(0, 0, 0, 153));
            auto const close_label_size = ImGui::CalcTextSize("X");
            draw->AddText(
                ImVec2{close_min.x + (close_size.x - close_label_size.x) * 0.5f, close_min.y + (close_size.y - close_label_size.y) * 0.5f},
                IM_COL32(255, 255, 255, 255),
                "X");

            if (channel_show_sampler[ch]) {
                ImGui::SetCursorScreenPos(slot_min);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{1.f, 1.f, 1.f, 1.f});
                ImGui::BeginChild("SamplerMenu", ImVec2{slot_w, slot_h}, true);
                auto filter = input->contains("sampler") && (*input)["sampler"].contains("filter")
                                  ? (*input)["sampler"]["filter"].get<std::string>()
                                  : "linear";
                auto wrap = input->contains("sampler") && (*input)["sampler"].contains("wrap")
                                ? (*input)["sampler"]["wrap"].get<std::string>()
                                : "repeat";
                if (ImGui::BeginCombo("Filter", filter.c_str())) {
                    for (auto const *opt : {"nearest", "linear", "mipmap"}) {
                        if (ImGui::Selectable(opt, filter == opt)) {
                            (*input)["sampler"]["filter"] = opt;
                            dirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::BeginCombo("Wrap", wrap.c_str())) {
                    for (auto const *opt : {"clamp", "repeat"}) {
                        if (ImGui::Selectable(opt, wrap == opt)) {
                            (*input)["sampler"]["wrap"] = opt;
                            dirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }

        ImGui::SetCursorScreenPos(ImVec2{slot_min.x, label_y});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{1.f, 1.f, 1.f, 1.f});
        ImGui::BeginChild("ChannelLabel", ImVec2{slot_w, label_h}, false, ImGuiWindowFlags_NoScrollbar);
        ImGui::Text("iChannel%d", ch);
        if (has_input) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(slot_w - 28.f);
            if (AppUi::s_instance != nullptr && AppUi::s_instance->icons.settings.tex_id != nullptr) {
                if (AppUi::s_instance->icons.image_button("##chset", AppUi::s_instance->icons.settings)) {
                    channel_show_sampler[ch] = !channel_show_sampler[ch];
                }
            } else if (ImGui::SmallButton("Set")) {
                channel_show_sampler[ch] = !channel_show_sampler[ch];
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

void BufferPanel::draw_bpiw_modal() {
    if (!show_bpiw) {
        return;
    }

    refresh_bpiw_preview_textures();

    ImGui::SetNextWindowSize(ImVec2{ui_layout::k_bpiw_window_w, ui_layout::k_bpiw_window_h}, ImGuiCond_Always);
    ImGui::OpenPopup("BpiwSelectInput");
    ImGuiWindowFlags const popup_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::BeginPopupModal("BpiwSelectInput", &show_bpiw, popup_flags)) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.933f, 0.933f, 0.933f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.455f, 0.455f, 0.455f, 1.f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{4.f, 0.f});
    int const tab_style = ui_theme::push_buffer_panel_style();

    if (ImGui::BeginTabBar("BpiwTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Misc")) {
            draw_bpiw_orange_separator();
            draw_bpiw_option_grid(misc_bpiw_options(), 0, [&](BpiwOption const &opt) { apply_bpiw_selection(opt.img_path, 0); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            draw_bpiw_orange_separator();
            draw_bpiw_option_grid(texture_bpiw_options(), 1, [&](BpiwOption const &opt) { apply_bpiw_selection(opt.img_path, 1); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cubemaps")) {
            draw_bpiw_orange_separator();
            draw_bpiw_option_grid(cubemap_bpiw_options(), 2, [&](BpiwOption const &opt) { apply_bpiw_selection(opt.img_path, 2); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Volumes")) {
            draw_bpiw_orange_separator();
            draw_bpiw_option_grid(volume_bpiw_options(), 3, [&](BpiwOption const &opt) { apply_bpiw_selection(opt.img_path, 3); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ui_theme::pop_buffer_panel_style(tab_style);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::EndPopup();
}

void BufferPanel::draw_compile_error_modal() {
    if (!show_compile_error_dialog || compile_message.empty()) {
        return;
    }

    ImGui::OpenPopup("ShaderCompileError");
    auto const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
    ImGui::SetNextWindowSize(ImVec2{680.f, 420.f}, ImGuiCond_Appearing);

    ImGuiWindowFlags const popup_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::BeginPopupModal("ShaderCompileError", &show_compile_error_dialog, popup_flags)) {
        return;
    }

    ImGui::TextUnformatted("Shader compilation failed");
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_theme::k_shader_input_body);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.75f, 0.1f, 0.1f, 1.f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{8.f, 8.f});
    ImGui::BeginChild("CompileErrorScroll", ImVec2{-1.f, -ImGui::GetFrameHeightWithSpacing() - 12.f}, true);
    if (AppUi::s_instance != nullptr && AppUi::s_instance->code_font != nullptr) {
        ImGui::PushFont(AppUi::s_instance->code_font);
    }
    ImGui::TextUnformatted(compile_message.c_str());
    if (AppUi::s_instance != nullptr && AppUi::s_instance->code_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 80.f) * 0.5f);
    if (ImGui::Button("Close", ImVec2{80.f, 0.f})) {
        show_compile_error_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void BufferPanel::draw(float panel_width, float panel_height) {
    auto const display_w = ImGui::GetIO().DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2{display_w - panel_width, 0.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2{panel_width, panel_height}, ImGuiCond_Always);

    int const style_count = ui_theme::push_buffer_panel_style();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.f, 1.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{4.f, 0.f});
    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("BufferPanel", nullptr, flags)) {
        draw_tabs_and_add();

        auto const body_h = ImGui::GetContentRegionAvail().y;
        ImGuiWindowFlags const body_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("BufferPanelBody", ImVec2{-1.f, body_h}, false, body_flags);

        draw_shader_input_section();

        bool const is_common = active_pass_is_common();
        float const channels_h = is_common ? 0.f : ui_layout::k_ichannel_row_height;
        auto const spacing = ImGui::GetStyle().ItemSpacing.y;
        float const footer_h = ui_layout::k_code_toolbar_height + channels_h + (channels_h > 0.f ? spacing : 0.f);
        float const editor_h = std::max(ImGui::GetContentRegionAvail().y - footer_h, 72.f);

        draw_code_editor(editor_h);
        draw_code_toolbar();

        if (!is_common) {
            ImGui::BeginChild("ChannelsRow", ImVec2{-1.f, channels_h}, false, body_flags);
            draw_channels();
            ImGui::EndChild();
        }

        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ui_theme::pop_buffer_panel_style(style_count);

    draw_bpiw_modal();
    draw_compile_error_modal();
}
