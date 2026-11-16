cmake_minimum_required(VERSION 3.25)
project(BetterRemotePlay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT WIN32)
    message(FATAL_ERROR "BetterRemotePlay is Windows-first; port the presenter before other platforms.")
endif()

add_library(brp_media STATIC
    src/platform/win32_window.cpp
    src/media/pipeline.cpp
)
target_include_directories(brp_media PUBLIC src)
target_compile_definitions(brp_media PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(brp_media PUBLIC d3d11 dxgi d3dcompiler)

if (MSVC)
    target_compile_options(brp_media PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(brp-bench tools/brp-bench/main.cpp)
target_link_libraries(brp-bench PRIVATE brp_media)

if (MSVC)
    target_compile_options(brp-bench PRIVATE /W4 /permissive- /utf-8)
endif()

# --- Application shell (Direct2D UI over the same D3D11 device) -------------

add_library(brp_ui STATIC
    src/ui/ui_renderer.cpp
)
target_include_directories(brp_ui PUBLIC src)
target_link_libraries(brp_ui PUBLIC brp_media d2d1 dwrite)

add_executable(brp
    src/main_app.cpp
    src/app/app.cpp
    src/session/profile.cpp
    src/platform/gamepad.cpp
)
target_link_libraries(brp PRIVATE brp_ui hid setupapi xinput)

# Diagnostic only: isolates D2D/D3D11 interop problems from app code.
add_executable(d2d_probe tools/d2d_probe/main.cpp)
target_link_libraries(d2d_probe PRIVATE d2d1 d3d11 dxgi)

if (MSVC)
    target_compile_options(brp_ui PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(brp PRIVATE /W4 /permissive- /utf-8)
endif()

# The real client will link the forked chiaki-ng session layer.
# vendor/chiaki-ng/lib/src is the transport/protocol core (session, ctrl, takion,
# fec, videoreceiver, audioreceiver, controller, discovery, regist, ...).
# setsu is the extracted protocol library. Both are vendored now so the tree is
# stable; the target that compiles them lands in Phase 2.
file(GLOB CHIAKI_CORE CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vendor/chiaki-ng/lib/src/*.c")
file(GLOB SETSU_CORE CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vendor/chiaki-ng/setsu/src/*.c")
if (CHIAKI_CORE)
    list(LENGTH CHIAKI_CORE chiaki_count)
    message(STATUS "chiaki-ng core vendored: ${chiaki_count} C sources -> brp-session (Phase 2)")
else()
    message(STATUS "chiaki-ng submodule not populated (git submodule update --init --depth 1)")
endif()
