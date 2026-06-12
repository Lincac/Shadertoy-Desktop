cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/deps.cmake")

project(desktop-shadertoy VERSION 0.4.4)

add_executable(${PROJECT_NAME}
    "src/main.cpp"
    "src/app/viewport.cpp"
    "src/app/resources.cpp"
    "src/ui/app_window.cpp"
    "src/ui/app_ui.cpp"
    "src/ui/ui_icons.cpp"
    "src/ui/components/buffer_panel.cpp"
)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0601)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(fmt CONFIG REQUIRED)
# boost-asio / boost-beast are header-only; vcpkg installs them via manifest.
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-nativefiledialog CONFIG REQUIRED)

find_package(glslang CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

set(DAXA_USE_VCPKG false)
set(DAXA_ENABLE_UTILS_IMGUI true)
set(DAXA_ENABLE_UTILS_MEM true)
set(DAXA_ENABLE_UTILS_PIPELINE_MANAGER_GLSLANG true)
set(DAXA_ENABLE_UTILS_PIPELINE_MANAGER_SPIRV_VALIDATION false)
set(DAXA_ENABLE_UTILS_TASK_GRAPH true)
set(DAXA_ENABLE_TESTS false)
add_subdirectory(${PROJECT_SOURCE_DIR}/deps/Daxa)

target_link_libraries(${PROJECT_NAME}
PRIVATE
    daxa::daxa
    nlohmann_json::nlohmann_json
    glfw
    imgui::imgui
    fmt::fmt
    unofficial::nativefiledialog::nfd
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${Stb_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    "${CMAKE_CURRENT_LIST_DIR}/src"
)

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
    )
    target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${OPENSSL_INCLUDE_DIR}
    )
else()
    target_link_libraries(${PROJECT_NAME}
    PRIVATE
        OpenSSL::SSL
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    configure_file("packaging/main.rc.in" "${CMAKE_BINARY_DIR}/main.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/main.rc")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_link_options(${PROJECT_NAME} PRIVATE "-Wl,/ENTRY:mainCRTStartup,/SUBSYSTEM:WINDOWS")
    else()
        target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup /SUBSYSTEM:WINDOWS)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE Dwmapi)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/packaging.cmake")
