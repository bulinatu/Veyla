#include "media/nv_reflex.h"

#include <cstdio>

namespace brp::media {
namespace {

// Public NVAPI interface ids (nvapi_QueryInterface offsets). A wrong id is
// safe: the lookup returns nullptr and Reflex stays unavailable.
constexpr unsigned kNvApiInitialize = 0x0150E828u;
constexpr unsigned kNvApiD3DSetSleepMode = 0xAC1CA9E0u;
constexpr unsigned kNvApiD3DSleep = 0x852CD1D2u;
constexpr unsigned kNvApiD3DSetLatencyMarker = 0xD9984C05u;

// NV_LATENCY_MARKER_TYPE
constexpr unsigned kSimulationStart = 0;
constexpr unsigned kSimulationEnd = 1;
constexpr unsigned kRenderSubmitStart = 2;
constexpr unsigned kRenderSubmitEnd = 3;
constexpr unsigned kPresentStart = 4;
constexpr unsigned kPresentEnd = 5;

}  // namespace

NvReflex::~NvReflex() {
    if (module_) {
        ::FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool NvReflex::Load(std::string& detail) {
    if (module_) {
        return available();
    }
    module_ = ::LoadLibraryW(L"nvapi64.dll");
    if (!module_) {
        detail = "nvapi64.dll not found";
        std::fprintf(stderr, "[reflex] %s\n", detail.c_str());
        return false;
    }
    const auto query =
        reinterpret_cast<QueryInterfaceFn>(::GetProcAddress(module_, "nvapi_QueryInterface"));
    if (!query) {
        detail = "nvapi64.dll has no nvapi_QueryInterface";
        return false;
    }
    initialize_ = reinterpret_cast<InitializeFn>(query(kNvApiInitialize));
    set_sleep_mode_ = reinterpret_cast<SetSleepModeFn>(query(kNvApiD3DSetSleepMode));
    sleep_ = reinterpret_cast<SleepFn>(query(kNvApiD3DSleep));
    set_latency_marker_ =
        reinterpret_cast<SetLatencyMarkerFn>(query(kNvApiD3DSetLatencyMarker));
    if (!initialize_ || !set_sleep_mode_ || !sleep_) {
        detail = "NVAPI Reflex entry points are not exposed by this driver";
        std::fprintf(stderr, "[reflex] %s\n", detail.c_str());
        return false;
    }
    const int status = initialize_();
    if (status != 0) {
        detail = "NvAPI_Initialize failed (" + std::to_string(status) + ")";
        std::fprintf(stderr, "[reflex] %s\n", detail.c_str());
        return false;
    }
    status_ = "Reflex available";
    std::fprintf(stderr, "[reflex] NVAPI loaded\n");
    return true;
}

bool NvReflex::SetMode(ID3D11Device* device, int mode, std::string& detail) {
    mode_ = mode;
    if (mode <= 0) {
        status_ = "Reflex off";
        if (device && set_sleep_mode_) {
            SetSleepModeParams params{};
            static_assert(sizeof(SetSleepModeParams) == 44,
                          "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
            params.version = (1u << 16) | sizeof(SetSleepModeParams);
            params.low_latency_mode = 0;
            set_sleep_mode_(device, &params);
        }
        return true;
    }
    if (!device) {
        detail = "no D3D device";
        mode_ = 0;
        return false;
    }
    // Measured on the reference machine: enabling the driver's low-latency
    // mode changes the present cadence so the waitable chain settles at
    // ~225 Hz with 56+ missed refreshes per second, and the 60 fps source
    // loses about four frames per second. That is worse than the latency it
    // could save on a remote stream, so the mode is not applied.
    detail = "The Reflex driver mode breaks the 240 Hz present clock";
    mode_ = 0;
    status_ = "Reflex disabled (would break pacing)";
    return false;
}

bool NvReflex::EnableMarkers(ID3D11Device* device, bool boost, std::string& detail) {
    if (!device) {
        detail = "no D3D device";
        return false;
    }
    if (!set_sleep_mode_ || !set_latency_marker_) {
        detail = "Reflex markers are not exposed by this driver";
        return false;
    }
    SetSleepModeParams params{};
    static_assert(sizeof(SetSleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
    params.version = (1u << 16) | sizeof(SetSleepModeParams);
    params.low_latency_mode = 0;  // no sleep stage; markers and boost only
    params.low_latency_boost = boost ? 1 : 0;
    params.minimum_interval_us = 0;
    params.use_markers = 1;
    params.use_min_queue_time = 0;
    const int status = set_sleep_mode_(device, &params);
    if (status != 0) {
        detail = "SetSleepMode(markers) failed (" + std::to_string(status) + ")";
        mode_ = 0;
        status_ = detail;
        std::fprintf(stderr, "[reflex] %s\n", detail.c_str());
        return false;
    }
    mode_ = 1;
    status_ = boost ? "Reflex markers + boost" : "Reflex markers";
    std::fprintf(stderr, "[reflex] markers enabled%s\n", boost ? " + boost" : "");
    return true;
}

void NvReflex::Mark(ID3D11Device* device, unsigned marker_type) {
    if (mode_ <= 0 || !device || !set_latency_marker_) {
        return;
    }
    LatencyMarkerParams params{};
    params.version = (1u << 16) | sizeof(LatencyMarkerParams);
    params.frame_id = frame_id_;
    params.marker_type = marker_type;
    set_latency_marker_(device, &params);
}

void NvReflex::BeginFrame(ID3D11Device* device) {
    ++frame_id_;
    Mark(device, kSimulationStart);
}

void NvReflex::EndSimulation(ID3D11Device* device) {
    Mark(device, kSimulationEnd);
}

void NvReflex::RenderSubmitStart(ID3D11Device* device) {
    Mark(device, kRenderSubmitStart);
}

void NvReflex::RenderSubmitEnd(ID3D11Device* device) {
    Mark(device, kRenderSubmitEnd);
}

void NvReflex::PresentStart(ID3D11Device* device) {
    Mark(device, kPresentStart);
}

void NvReflex::PresentEnd(ID3D11Device* device) {
    Mark(device, kPresentEnd);
}

void NvReflex::Sleep(ID3D11Device* device) {
    if (mode_ > 0 && device && sleep_ && set_latency_marker_) {
        sleep_(device);
    }
}

}  // namespace brp::media
