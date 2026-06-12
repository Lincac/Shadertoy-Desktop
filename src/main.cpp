#include <app/viewport.hpp>
#include <app/resources.hpp>

#include <cstdint>
#include <iostream>

#include <daxa/daxa.hpp>
#include <daxa/utils/pipeline_manager.hpp>
#include <daxa/utils/task_graph.hpp>

#include <ui/app_ui.hpp>
#include <ui/layout.hpp>

#include <fstream>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct ShaderApp {
    daxa::Instance daxa_instance;
    daxa::Device daxa_device;
    // ui / viewport 持有 Device 副本；TaskGraph 持有 Swapchain 副本。
    // 声明顺序决定逆序析构：先 TaskGraph，再 UI/Viewport，最后 Device。
    AppUi ui;
    Viewport viewport;
    daxa::TaskImage task_swapchain_image;
    daxa::TaskGraph main_task_graph;

    daxa_f32vec2 recorded_viewport_client{-1.f, -1.f};

    ShaderApp();
    ~ShaderApp();

    ShaderApp(const ShaderApp &) = delete;
    ShaderApp(ShaderApp &&) = delete;
    auto operator=(const ShaderApp &) -> ShaderApp & = delete;
    auto operator=(ShaderApp &&) -> ShaderApp & = delete;

    void update();
    auto should_close() -> bool;
    void render();
    void download_shadertoy(std::string const &input);
    void reload_shaders_from_panel();
    auto record_main_task_graph() -> daxa::TaskGraph;

    [[nodiscard]] auto effective_viewport_size() const -> daxa_f32vec2;
    void rebuild_task_graph_if_ready();
};

namespace core {
    void log_error(std::string const &msg) {
        std::cerr << msg << std::endl;
    }
} // namespace core

void search_for_path_to_fix_working_directory(std::span<std::filesystem::path const> test_paths) {
    auto current_path = std::filesystem::current_path();
    while (true) {
        for (auto const &test_path : test_paths) {
            if (std::filesystem::exists(current_path / test_path)) {
                std::filesystem::current_path(current_path);
                return;
            }
        }
        if (!current_path.has_parent_path()) {
            break;
        }
        current_path = current_path.parent_path();
    }
}

char const *shaderToyDomain = "www.shadertoy.com";
char const *shaderToyPort = "443";
char const *shaderToyKey = "Bt8jhH";
char const *userAgent = BOOST_BEAST_VERSION_STRING;

struct ShadertoyApi {
    using Stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    boost::beast::net::io_context ioc{};
    boost::asio::ip::tcp::resolver resolver{ioc};
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::method::sslv23_client};
    boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> results = resolver.resolve(shaderToyDomain, shaderToyPort);

    ShadertoyApi() = default;
    auto operator=(const ShadertoyApi &) -> ShadertoyApi & = delete;
    auto operator=(ShadertoyApi &&) -> ShadertoyApi & = delete;
    ShadertoyApi(const ShadertoyApi &) = delete;
    ShadertoyApi(ShadertoyApi &&) = delete;
    ~ShadertoyApi() = default;

    auto request(std::string const &requestURI) -> std::string {
        Stream stream = Stream(ioc, ssl_ctx);
        boost::beast::get_lowest_layer(stream).connect(results.begin(), results.end());
        stream.handshake(boost::asio::ssl::stream_base::client);

        auto req = boost::beast::http::request<boost::beast::http::string_body>{boost::beast::http::verb::post, requestURI, 10};
        req.set(boost::beast::http::field::host, shaderToyDomain);
        req.set(boost::beast::http::field::user_agent, userAgent);
        boost::beast::http::write(stream, req);
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::dynamic_body> res;
        boost::beast::http::read(stream, buffer, res);

        boost::beast::error_code ec;
        stream.shutdown(ec);
        if (ec) {
            core::log_error(ec.message());
        }

        return boost::beast::buffers_to_string(res.body().data());
    }
};

auto download_shadertoy_json_from_id_or_url(std::string const &input, ShadertoyApi &api) -> nlohmann::json {
    auto shader_id = input;
    auto slash_pos = shader_id.find_last_of('/');
    if (slash_pos != std::string::npos) {
        shader_id = shader_id.substr(slash_pos + 1);
    }

    return nlohmann::json::parse(api.request("/api/v1/shaders/" + shader_id + "?key=" + shaderToyKey));
}

auto main() -> int {
    search_for_path_to_fix_working_directory(std::array{
        std::filesystem::path{"media"},
    });

    auto app = ShaderApp();
    while (true) {
        app.update();
        if (app.should_close()) {
            break;
        }
        app.render();
    }
}

ShaderApp::ShaderApp()
    : daxa_instance{daxa::create_instance({})},
      daxa_device{[&]() {
          auto required_implicit = daxa::ImplicitFeatureFlagBits::SWAPCHAIN;
          auto device_info = daxa::DeviceInfo2{};
          device_info.name = "Desktop Shadertoy";
          device_info.explicit_features = daxa::ExplicitFeatureFlagBits::ROBUSTNESS_2;
          device_info = daxa_instance.choose_device(required_implicit, device_info);
          return daxa_instance.create_device_2(device_info);
      }()},
      ui{daxa_device},
      viewport{daxa_device},
      task_swapchain_image{daxa::TaskImageInfo{.swapchain_image = true}} {
    ui.app_window.on_resize = [&]() { rebuild_task_graph_if_ready(); };
    ui.app_window.on_drop = [&](std::span<char const *> paths) {
        ui.buffer_panel.load_shadertoy_json(nlohmann::json::parse(std::ifstream(paths[0])));
    };
    ui.app_window.on_mouse_move = std::bind(&Viewport::on_mouse_move, &viewport, std::placeholders::_1, std::placeholders::_2);
    ui.app_window.on_mouse_button = std::bind(&Viewport::on_mouse_button, &viewport, std::placeholders::_1, std::placeholders::_2);
    ui.app_window.on_key = [&](int32_t key_id, int32_t action) {
        viewport.on_key(key_id, action);
    };

    ui.on_reset = std::bind(&Viewport::reset, &viewport);
    ui.on_toggle_pause = std::bind(&Viewport::on_toggle_pause, &viewport, std::placeholders::_1);

    ui.on_toggle_fullscreen = [&](bool /*is_fullscreen*/) {
        ui.app_window.set_fullscreen(ui.is_fullscreen);
        viewport.reset();
    };

    ui.on_download = [&](std::string const &input) {
        download_shadertoy(input);
    };

    ui.buffer_panel.on_recompile = [this]() { reload_shaders_from_panel(); };

    ui.buffer_panel.load_shadertoy_json(nlohmann::json::parse(std::ifstream(resource_dir / "default-shader.json")));
    reload_shaders_from_panel();
}

ShaderApp::~ShaderApp() {
    daxa_device.wait_idle();
    // TaskGraph 持有 Swapchain 引用，必须先于 UI Swapchain 释放。
    main_task_graph = daxa::TaskGraph{};
    task_swapchain_image = daxa::TaskImage{};
    daxa_device.wait_idle();

    viewport.shutdown();
    ui.shutdown();

    daxa_device.wait_idle();
    daxa_device.collect_garbage();
    daxa_device = {};
}

void ShaderApp::update() {
    if (!ui.paused) {
        viewport.update();
    }
    ui.update(viewport.gpu_input.Time, viewport.last_known_fps);
}

void ShaderApp::reload_shaders_from_panel() {
    ui.buffer_panel.sync_all_pass_codes_to_json();
    viewport.load_shadertoy_json(ui.buffer_panel.get_shadertoy_json());
    ui.buffer_panel.dirty = false;

    if (!viewport.last_compile_error.empty()) {
        ui.buffer_panel.compile_message = viewport.last_compile_error;
        while (!ui.buffer_panel.compile_message.empty() &&
               (ui.buffer_panel.compile_message.back() == '\n' || ui.buffer_panel.compile_message.back() == '\r')) {
            ui.buffer_panel.compile_message.pop_back();
        }
        ui.buffer_panel.show_compile_error_dialog = true;
    } else {
        ui.buffer_panel.compile_message.clear();
        ui.buffer_panel.show_compile_error_dialog = false;
    }

    if (ui.app_window.size.x <= 0 || ui.app_window.size.y <= 0) {
        return;
    }

    rebuild_task_graph_if_ready();
}

auto ShaderApp::effective_viewport_size() const -> daxa_f32vec2 {
    auto const layout = ui.get_layout();
    if (ui.is_fullscreen) {
        return {
            static_cast<daxa_f32>(ui.app_window.size.x),
            static_cast<daxa_f32>(ui.app_window.size.y),
        };
    }
    return {layout.viewport_width(), layout.viewport_height()};
}

void ShaderApp::rebuild_task_graph_if_ready() {
    if (ui.app_window.size.x <= 0 || ui.app_window.size.y <= 0) {
        recorded_viewport_client = {-1.f, -1.f};
        return;
    }
    main_task_graph = record_main_task_graph();
    recorded_viewport_client = effective_viewport_size();
}

void ShaderApp::render() {
    auto &app_window = ui.app_window;
    if (app_window.size.x <= 0 || app_window.size.y <= 0) {
        return;
    }

    if (ui.buffer_panel.dirty) {
        reload_shaders_from_panel();
    }

    auto const cur_vp = effective_viewport_size();
    bool const vp_ready = cur_vp.x >= 1.f && cur_vp.y >= 1.f;
    bool const recorded_valid = recorded_viewport_client.x >= 0.f;
    bool const need_record =
        !recorded_valid ||
        (vp_ready && (std::abs(cur_vp.x - recorded_viewport_client.x) > 0.5f ||
                      std::abs(cur_vp.y - recorded_viewport_client.y) > 0.5f));
    if (need_record) {
        main_task_graph = record_main_task_graph();
        if (vp_ready) {
            recorded_viewport_client = cur_vp;
        }
    }

    auto const swapchain_image = app_window.swapchain.acquire_next_image();
    if (swapchain_image.is_empty()) {
        return;
    }
    task_swapchain_image.set_images({.images = {&swapchain_image, 1}});

    if (ui.paused) {
        viewport.gpu_input.TimeDelta = 0.f;
    } else {
        viewport.render();
    }

    main_task_graph.execute({});
    daxa_device.collect_garbage();

    if (!ui.paused) {
        ++viewport.gpu_input.Frame;
    }
}

auto ShaderApp::should_close() -> bool {
    return ui.should_close.load();
}

void ShaderApp::download_shadertoy(std::string const &input) {
    auto shadertoy_api = ShadertoyApi{};
    auto json = download_shadertoy_json_from_id_or_url(input, shadertoy_api);

    if (json.contains("Error")) {
        auto error = std::string{json["Error"]};
        if (error == "Shader not found") {
            core::log_error("Failed to download from shadertoy: " + error + ". This is usually because the creator does not allow API downloads on their shader");
        } else {
            core::log_error("Failed to download from shadertoy: " + error);
        }
        return;
    }

    if (ui.settings.export_downloads) {
        auto f = std::ofstream("test-shader.json");
        f << std::setw(4) << json["Shader"];
    }

    ui.buffer_panel.load_shadertoy_json(json["Shader"]);
}

auto ShaderApp::record_main_task_graph() -> daxa::TaskGraph {
    auto const layout = ui.get_layout();
    auto const viewport_size = effective_viewport_size();
    auto const is_fullscreen = ui.is_fullscreen;

    viewport.gpu_input.Resolution = daxa_f32vec3{
        static_cast<daxa_f32>(viewport_size.x),
        static_cast<daxa_f32>(viewport_size.y),
        1.0f,
    };
    viewport.gpu_input.ChannelResolution[0] = daxa_f32vec3{
        static_cast<daxa_f32>(viewport_size.x),
        static_cast<daxa_f32>(viewport_size.y),
        1.0f,
    };

    auto &app_window = ui.app_window;

    auto task_graph = daxa::TaskGraph(daxa::TaskGraphInfo{
        .device = daxa_device,
        .swapchain = app_window.swapchain,
        .name = "main_tg",
    });
    task_graph.use_persistent_image(task_swapchain_image);

    task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_swapchain_image),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            auto &recorder = ti.recorder;
            auto swapchain_image = ti.get(task_swapchain_image).ids[0];
            auto swapchain_image_full_slice = daxa_device.image_view_info(swapchain_image.default_view()).value().slice;
            recorder.clear_image({
                .dst_image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                .clear_value = std::array<daxa::f32, 4>{0.2f, 0.1f, 0.4f, 1.0f},
                .dst_image = swapchain_image,
                .dst_slice = swapchain_image_full_slice,
            });
        },
        .name = "clear screen",
    });

    auto viewport_render_image = viewport.record(task_graph);

    task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_READ, daxa::ImageViewType::REGULAR_2D, viewport_render_image),
            daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_swapchain_image),
        },
        .task = [this, viewport_render_image, viewport_size, layout, is_fullscreen](daxa::TaskInterface const &ti) {
            auto &recorder = ti.recorder;
            auto const image_size = ti.device.image_info(ti.get(viewport_render_image).ids[0]).value().size;
            auto const sw = static_cast<int32_t>(image_size.x);
            auto const sh = static_cast<int32_t>(image_size.y);
            if (sw <= 0 || sh <= 0) {
                return;
            }
            std::array<daxa::Offset3D, 2> dst_offsets{};
            if (is_fullscreen) {
                auto const swapchain_size = ti.device.image_info(ti.get(task_swapchain_image).ids[0]).value().size;
                dst_offsets = {
                    daxa::Offset3D{0, 0, 0},
                    daxa::Offset3D{static_cast<daxa::i32>(swapchain_size.x), static_cast<daxa::i32>(swapchain_size.y), 1},
                };
            } else {
                auto const vp_pos = layout.viewport_pos();
                auto const viewport_pos1 = daxa_f32vec2{vp_pos.x + viewport_size.x, vp_pos.y + viewport_size.y};
                dst_offsets = {
                    daxa::Offset3D{static_cast<daxa::i32>(vp_pos.x), static_cast<daxa::i32>(vp_pos.y), 0},
                    daxa::Offset3D{static_cast<daxa::i32>(viewport_pos1.x), static_cast<daxa::i32>(viewport_pos1.y), 1},
                };
            }
            recorder.blit_image_to_image({
                .src_image = ti.get(viewport_render_image).ids[0],
                .src_image_layout = ti.get(viewport_render_image).layout,
                .dst_image = ti.get(task_swapchain_image).ids[0],
                .dst_image_layout = ti.get(task_swapchain_image).layout,
                .src_offsets = {{{0, sh, 0}, {sw, 0, 1}}},
                .dst_offsets = dst_offsets,
                .filter = daxa::Filter::LINEAR,
            });
        },
        .name = "blit_image_to_image",
    });

    if (!ui.is_fullscreen) {
        task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskImageAccess::COLOR_ATTACHMENT, daxa::ImageViewType::REGULAR_2D, task_swapchain_image),
            },
            .task = [this](daxa::TaskInterface ti) {
                auto &recorder = ti.recorder;
                ui.render(recorder, ti.get(task_swapchain_image).ids[0]);
            },
            .name = "ui draw",
        });
    }
    task_graph.submit({});
    task_graph.present({});
    task_graph.complete({});
    return task_graph;
}
