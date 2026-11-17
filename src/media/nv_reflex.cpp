#include "media/nv_reflex.h"

#include <cstdio>

namespace brp::media {
namespace {

// Public NVAPI interface ids (nvapi_QueryInterface offsets). A wrong id is
// safe: the lookup returns nullptr and Reflex stays unavailable.
constexpr unsigned kNvApiInitialize = 0x0150E828u;
constexpr unsigned kNvApiD3DSetSleepMode = 0xAC1CA9E0u;
constexpr unsigned kNvApiD3DSleep = 0x852CD1D2u;

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
    if (!initialize_ || !set_sleep_mode_ || !sleep_) {
        detail = "NVAPI Reflex entry points are not exposed by this driver";
        return false;
    }
    const int status = initialize_();
    if (status != 0) {
        detail = "NvAPI_Initialize failed (" + std::to_string(status) + ")";
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
            static_assert(sizeof(SetSleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
    params.version = (1u << 16) | sizeof(SetSleepModeParams);
            params.low_latency_mode = 0;
            set_sleep_mode_(device, &params);
        }
        return true;
    }
    if (!device) {
        detail = "no D3D device";
        return false;
    }
    if (!available()) {
        detail = "Reflex is not available";
        return false;
    }
    SetSleepModeParams params{};
    static_assert(sizeof(SetSleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
    params.version = (1u << 16) | sizeof(SetSleepModeParams);
    params.low_latency_mode = 1;
    params.low_latency_boost = mode >= 2 ? 1u : 0u;
    params.use_markers = 0;
    params.minimum_interval_us = 0;
    const int status = set_sleep_mode_(device, &params);
    if (status != 0) {
        detail = "NvAPI_D3D_SetSleepMode failed (" + std::to_string(status) + ")";
        mode_ = 0;
        status_ = "Reflex off";
        return false;
    }
    status_ = mode >= 2 ? "Reflex on + boost" : "Reflex on";
    std::fprintf(stderr, "[reflex] %s\n", status_.c_str());
    return true;
}

void NvReflex::Sleep(ID3D11Device* device) {
    if (mode_ > 0 && device && sleep_) {
        sleep_(device);
    }
}

}  // namespace brp::media
