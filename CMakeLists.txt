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
    src/audio/audio_engine.cpp
    src/net/reachability.cpp
    src/session/discovery.cpp
)
target_link_libraries(brp PRIVATE brp_ui brp_session hid setupapi xinput ole32 avrt uuid ws2_32)

# Diagnostic only: isolates D2D/D3D11 interop problems from app code.
add_executable(d2d_probe tools/d2d_probe/main.cpp)
target_link_libraries(d2d_probe PRIVATE d2d1 d3d11 dxgi)

if (MSVC)
    target_compile_options(brp_ui PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(brp PRIVATE /W4 /permissive- /utf-8)
endif()

# --- Session layer: chiaki-ng registration subset -------------------------
# Only the PIN/PSN registration path, no decoder and no protobuf. mbedTLS is
# fetched and built as a static dependency (chiaki-ng's Windows configuration).
include(FetchContent)
# mbedTLS 2.28 declares cmake_minimum_required(3.1); CMake 4 rejects it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG 8b3f26a5ac38d4fdccbc5c5366229f3e01dafcc0  # v2.28.0
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(mbedtls)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/chiaki-gen/chiaki")
file(WRITE "${CMAKE_BINARY_DIR}/chiaki-gen/chiaki/config.h"
"#pragma once
#define CHIAKI_LIB_ENABLE_OPUS 0
#define CHIAKI_LIB_ENABLE_PI_DECODER 0
#define CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0
")

set(CHIAKI_ROOT "${CMAKE_SOURCE_DIR}/vendor/chiaki-ng")

add_library(brp_session STATIC
    ${CHIAKI_ROOT}/lib/src/common.c
    ${CHIAKI_ROOT}/lib/src/base64.c
    ${CHIAKI_ROOT}/lib/src/log.c
    ${CHIAKI_ROOT}/lib/src/thread.c
    ${CHIAKI_ROOT}/lib/src/time.c
    ${CHIAKI_ROOT}/lib/src/random.c
    ${CHIAKI_ROOT}/lib/src/sock.c
    ${CHIAKI_ROOT}/lib/src/http.c
    ${CHIAKI_ROOT}/lib/src/rpcrypt.c
    ${CHIAKI_ROOT}/lib/src/ecdh.c
    ${CHIAKI_ROOT}/lib/src/regist.c
    ${CHIAKI_ROOT}/lib/src/stoppipe.c
    ${CHIAKI_ROOT}/lib/src/remote/rudp.c
    ${CHIAKI_ROOT}/lib/src/remote/rudpsendbuffer.c
    src/session/rp_strings.c
    src/session/aia_stub.c
    src/session/pairing.cpp
)
target_include_directories(brp_session PUBLIC
    "${CMAKE_SOURCE_DIR}/src/session/shim"
    "${CHIAKI_ROOT}/lib/include"
    "${CHIAKI_ROOT}/lib/src"
    "${CMAKE_BINARY_DIR}/chiaki-gen"
    src
)
target_compile_definitions(brp_session PUBLIC CHIAKI_LIB_ENABLE_MBEDTLS)
target_link_libraries(brp_session PUBLIC mbedtls mbedx509 mbedcrypto ws2_32 bcrypt)
set_target_properties(brp_session PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

target_compile_options(brp_session PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zi>)

add_executable(discover_probe tools/discover_probe/main.cpp src/session/discovery.cpp)
target_include_directories(discover_probe PRIVATE src)
target_link_libraries(discover_probe PRIVATE ws2_32)

add_executable(pair_probe tools/pair_probe/main.cpp src/session/profile.cpp)
target_link_libraries(pair_probe PRIVATE brp_session)
target_compile_options(pair_probe PRIVATE /Zi)
target_link_options(pair_probe PRIVATE /DEBUG)

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
