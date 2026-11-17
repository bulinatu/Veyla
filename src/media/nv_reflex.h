#pragma once

#include <windows.h>

#include <d3d11.h>

#include <string>

namespace brp::media {

// NVIDIA Reflex low-latency mode for this app's own present chain.
//
// Reflex is not only a game feature: NVAPI exposes the same sleep mode any
// D3D application can use (NvAPI_D3D_SetSleepMode / NvAPI_D3D_Sleep). This is
// the real driver entry point, resolved from nvapi64.dll at runtime; nothing
// here is emulated. It cannot touch the console's input latency.
class NvReflex {
public:
    NvReflex() = default;
    ~NvReflex();

    NvReflex(const NvReflex&) = delete;
    NvReflex& operator=(const NvReflex&) = delete;

    // Loads nvapi64.dll and resolves the three Reflex entry points.
    bool Load(std::string& detail);

    // 0 off, 1 on, 2 on + boost.
    bool SetMode(ID3D11Device* device, int mode, std::string& detail);

    // Call once per frame, right before presenting.
    void Sleep(ID3D11Device* device);

    bool available() const { return set_sleep_mode_ != nullptr && sleep_ != nullptr; }
    int mode() const { return mode_; }
    const std::string& status() const { return status_; }

private:
    using QueryInterfaceFn = void*(__cdecl*)(unsigned int);
    using InitializeFn = int(__cdecl*)();
    // NV_SET_SLEEP_MODE_PARAMS_V1: NvU32, then NvBool (NvU8) pairs, then
    // NvU32, then the reserved tail. Layout matters: the driver checks the
    // version word, which is sizeof(struct) | (1 << 16).
    struct SetSleepModeParams {
        unsigned version;
        unsigned char low_latency_mode;
        unsigned char low_latency_boost;
        unsigned minimum_interval_us;
        unsigned char use_markers;
        unsigned char use_min_queue_time;
        unsigned char reserved[30];
    };
    using SetSleepModeFn = int(__cdecl*)(IUnknown*, SetSleepModeParams*);
    using SleepFn = int(__cdecl*)(IUnknown*);

    HMODULE module_ = nullptr;
    InitializeFn initialize_ = nullptr;
    SetSleepModeFn set_sleep_mode_ = nullptr;
    SleepFn sleep_ = nullptr;
    int mode_ = 0;
    std::string status_;
};

}  // namespace brp::media
