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
    src/media/nv_vfx.cpp
    src/media/nv_dlss_sr.cpp
    src/media/nv_dlssnr.cpp
    src/media/nv_reflex.cpp
    src/media/rgba_scale.cpp
)
target_include_directories(brp_media PUBLIC src)
target_include_directories(brp_media PRIVATE third_party/nvidia-ngx/include)
target_compile_definitions(brp_media PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(brp_media PUBLIC d3d11 dxgi d3dcompiler d3d12 dwmapi)

if (MSVC)
    target_compile_options(brp_media PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(brp-bench tools/brp-bench/main.cpp)
target_link_libraries(brp-bench PRIVATE brp_media)

if (MSVC)
    target_compile_options(brp-bench PRIVATE /W4 /permissive- /utf-8)
endif()

# --- RIFE (TensorRT, loaded at run time; no CUDA/TensorRT SDK link) ----------
add_library(brp_rife STATIC src/media/rife.cpp)
target_include_directories(brp_rife PUBLIC src)
target_include_directories(brp_rife PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/tensorrt/include"
    "${CMAKE_SOURCE_DIR}/third_party/tensorrt/cuda_shim")
if (MSVC)
    target_compile_options(brp_rife PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(rife_probe tools/rife_probe/main.cpp)
target_link_libraries(rife_probe PRIVATE brp_rife)
if (MSVC)
    target_compile_options(rife_probe PRIVATE /W4 /permissive- /utf-8)
endif()
add_executable(vfx_probe tools/vfx_probe/main.cpp)
target_link_libraries(vfx_probe PRIVATE brp_media)
target_compile_options(vfx_probe PRIVATE /Zi)
target_link_options(vfx_probe PRIVATE /DEBUG)

add_executable(vfg_pace_probe tools/vfg_pace_probe/main.cpp)
target_link_libraries(vfg_pace_probe PRIVATE brp_media)
target_compile_options(vfg_pace_probe PRIVATE /Zi)
target_link_options(vfg_pace_probe PRIVATE /DEBUG)

add_executable(dlss_sr_module_probe tools/dlss_sr_module_probe/main.cpp)
target_link_libraries(dlss_sr_module_probe PRIVATE brp_media)

add_executable(nr_module_probe tools/nr_module_probe/main.cpp)
target_link_libraries(nr_module_probe PRIVATE brp_media)

add_executable(nr_motion_probe tools/nr_motion_probe/main.cpp)
target_link_libraries(nr_motion_probe PRIVATE brp_media)

add_executable(nr_sr_chain_probe tools/nr_sr_chain_probe/main.cpp)
target_link_libraries(nr_sr_chain_probe PRIVATE brp_media)

add_executable(nr_fg_probe tools/nr_fg_probe/main.cpp)
target_link_libraries(nr_fg_probe PRIVATE brp_media)
target_compile_options(dlss_sr_module_probe PRIVATE /Zi)
target_link_options(dlss_sr_module_probe PRIVATE /DEBUG)

add_executable(vsr_probe tools/vsr_probe/main.cpp)
target_link_libraries(vsr_probe PRIVATE brp_media)
target_compile_options(vsr_probe PRIVATE /Zi)
target_link_options(vsr_probe PRIVATE /DEBUG)

# Diagnostic only: DLSSNR signed-snippet host probe. Needs the NGX SDK headers;
# disabled unless NGX_SDK_INC points at a folder containing nvsdk_ngx.h.
set(NGX_SDK_INC "" CACHE PATH "NGX SDK include directory (nvsdk_ngx.h)")
if(NGX_SDK_INC AND EXISTS "${NGX_SDK_INC}/nvsdk_ngx.h")
    add_executable(dlssnr_probe tools/dlssnr_probe/main.cpp)
    target_include_directories(dlssnr_probe PRIVATE "${NGX_SDK_INC}")
    target_link_libraries(dlssnr_probe PRIVATE d3d11 d3d12 dxgi)
    target_compile_options(dlssnr_probe PRIVATE /Zi)
    target_link_options(dlssnr_probe PRIVATE /DEBUG)

    add_executable(dlss_sr_probe tools/dlss_sr_probe/main.cpp)
    target_include_directories(dlss_sr_probe PRIVATE "${NGX_SDK_INC}")
    target_link_libraries(dlss_sr_probe PRIVATE d3d11 dxgi)
    target_compile_options(dlss_sr_probe PRIVATE /Zi)
    target_link_options(dlss_sr_probe PRIVATE /DEBUG)

    add_executable(dlss_sr_d3d12_probe tools/dlss_sr_d3d12_probe/main.cpp)
    target_include_directories(dlss_sr_d3d12_probe PRIVATE "${NGX_SDK_INC}")
    target_link_libraries(dlss_sr_d3d12_probe PRIVATE d3d11 d3d12 dxgi)
    target_compile_options(dlss_sr_d3d12_probe PRIVATE /Zi)
    target_link_options(dlss_sr_d3d12_probe PRIVATE /DEBUG)
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
    src/session/psn_auth.cpp
    src/session/streaming.cpp
)
# GUI subsystem: no console window on launch. main() is still the entry point.
set_target_properties(brp PROPERTIES WIN32_EXECUTABLE TRUE)
if (MSVC)
    target_link_options(brp PRIVATE /ENTRY:mainCRTStartup)
endif()
target_link_libraries(brp PRIVATE brp_ui brp_decode brp_session hid setupapi xinput ole32 avrt uuid ws2_32 winhttp shell32 bcrypt winmm)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf")
    add_custom_command(TARGET brp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:brp>/fonts")
endif()

# The NGX feature DLL must sit next to the executable for Magpie-style NGX
# feature discovery. The runtime is user-supplied and gitignored.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nvidia-ngx/runtime/nvngx_dlss.dll")
    add_custom_command(TARGET brp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/nvidia-ngx/runtime/nvngx_dlss.dll"
            "$<TARGET_FILE_DIR:brp>/nvngx_dlss.dll")
endif()

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
    ${CHIAKI_ROOT}/lib/src/session.c
    ${CHIAKI_ROOT}/lib/src/takion.c
    ${CHIAKI_ROOT}/lib/src/ctrl.c
    ${CHIAKI_ROOT}/lib/src/streamconnection.c
    ${CHIAKI_ROOT}/lib/src/videoreceiver.c
    ${CHIAKI_ROOT}/lib/src/audioreceiver.c
    ${CHIAKI_ROOT}/lib/src/frameprocessor.c
    ${CHIAKI_ROOT}/lib/src/reorderqueue.c
    ${CHIAKI_ROOT}/lib/src/congestioncontrol.c
    ${CHIAKI_ROOT}/lib/src/takionsendbuffer.c
    ${CHIAKI_ROOT}/lib/src/packetstats.c
    ${CHIAKI_ROOT}/lib/src/audio.c
    ${CHIAKI_ROOT}/lib/src/bitstream.c
    ${CHIAKI_ROOT}/lib/src/gkcrypt.c
    ${CHIAKI_ROOT}/lib/src/senkusha.c
    ${CHIAKI_ROOT}/lib/src/launchspec.c
    ${CHIAKI_ROOT}/lib/src/discovery.c
    ${CHIAKI_ROOT}/lib/src/discoveryservice.c
    ${CMAKE_SOURCE_DIR}/build/gen/takion.pb.c    ${CHIAKI_ROOT}/lib/src/feedback.c
    ${CHIAKI_ROOT}/lib/src/feedbacksender.c
    ${CHIAKI_ROOT}/lib/src/controller.c
    ${CHIAKI_ROOT}/lib/src/orientation.c
    ${CHIAKI_ROOT}/third-party/nanopb/pb_encode.c
    ${CHIAKI_ROOT}/third-party/nanopb/pb_decode.c
    ${CHIAKI_ROOT}/third-party/nanopb/pb_common.c    src/session/rp_strings.c
    src/session/aia_stub.c
    src/session/pairing.cpp
)
target_include_directories(brp_session PUBLIC
    "${CMAKE_SOURCE_DIR}/src/session/shim"
    "${CHIAKI_ROOT}/lib/include"
    "${CHIAKI_ROOT}/lib/src"
    "${CMAKE_BINARY_DIR}/chiaki-gen"
    "${CMAKE_SOURCE_DIR}/build/gen"
    src
)
target_compile_definitions(brp_session PUBLIC CHIAKI_LIB_ENABLE_MBEDTLS)
target_link_libraries(brp_session PUBLIC mbedtls mbedx509 mbedcrypto ws2_32 bcrypt)
set_target_properties(brp_session PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

target_compile_options(brp_session PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zi>)

add_executable(discover_probe tools/discover_probe/main.cpp src/session/discovery.cpp)
target_include_directories(discover_probe PRIVATE src)
target_link_libraries(discover_probe PRIVATE brp_session)

add_executable(pair_probe tools/pair_probe/main.cpp src/session/profile.cpp)
target_link_libraries(pair_probe PRIVATE brp_session)
target_compile_options(pair_probe PRIVATE /Zi)
target_link_options(pair_probe PRIVATE /DEBUG)

add_executable(wake_probe tools/wake_probe/main.cpp src/session/profile.cpp
    src/session/discovery.cpp)
target_include_directories(wake_probe PRIVATE src)
target_link_libraries(wake_probe PRIVATE brp_session)

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

# --- Decode path (libavcodec) ---------------------------------------------
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")
if(EXISTS "${FFMPEG_ROOT}/lib/avcodec.lib")
    add_library(brp_decode STATIC src/media/decoder.cpp)
    target_include_directories(brp_decode PUBLIC src "${FFMPEG_ROOT}/include")
    target_link_libraries(brp_decode PUBLIC
        "${FFMPEG_ROOT}/lib/avcodec.lib"
        "${FFMPEG_ROOT}/lib/avformat.lib"
        "${FFMPEG_ROOT}/lib/avutil.lib"
        "${FFMPEG_ROOT}/lib/swscale.lib"
        "${FFMPEG_ROOT}/lib/swresample.lib")
    add_executable(decode_probe tools/decode_probe/main.cpp)
    target_link_libraries(decode_probe PRIVATE brp_decode)
    add_executable(scale_probe tools/scale_probe/main.cpp)
    target_link_libraries(scale_probe PRIVATE brp_decode)
    add_executable(mv_dump tools/mv_dump/main.cpp)
    target_link_libraries(mv_dump PRIVATE brp_decode)
    add_custom_command(TARGET decode_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:decode_probe>")
    add_custom_command(TARGET mv_dump POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:mv_dump>")
endif()

# --- chiaki streaming dependencies (nanopb runtime, jerasure, gf-complete) ---
target_include_directories(brp_session PUBLIC
    "${CHIAKI_ROOT}/third-party/nanopb"
    "${CHIAKI_ROOT}/third-party/jerasure/include"
    "${CHIAKI_ROOT}/third-party/jerasure/src"
    "${CHIAKI_ROOT}/third-party/gf-complete/include"
    "${CHIAKI_ROOT}/third-party/gf-complete/src")
file(GLOB BRP_GF_SRC "${CHIAKI_ROOT}/third-party/gf-complete/src/*.c")
file(GLOB BRP_JER_SRC "${CHIAKI_ROOT}/third-party/jerasure/src/*.c")
target_sources(brp_session PRIVATE ${BRP_GF_SRC} ${BRP_JER_SRC})
target_sources(brp_session PRIVATE "${CMAKE_SOURCE_DIR}/src/session/holepunch_stub.c")
target_sources(brp_session PRIVATE "${CHIAKI_ROOT}/lib/src/fec.c")
