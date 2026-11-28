#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/nv_of.h"

namespace brp::media {

// Native, in-process DLSS Frame Generation on D3D12.
//
// This is the replacement for the external NeuralScreen worker: no second
// process, no injected SM86 proxy, no topmost window. The feature is evaluated
// offscreen and writes a real frame plus N generated frames into shared
// textures that the DX12 video surface presents in order.
//
// A video stream has no engine motion vectors. The first version feeds zero
// depth and zero motion; NVOFA motion is the next step.
class NvDlssFg {
public:
    struct Frame {
        HANDLE shared = nullptr;
        bool generated = false;
    };

    NvDlssFg();
    ~NvDlssFg();

    NvDlssFg(const NvDlssFg&) = delete;
    NvDlssFg& operator=(const NvDlssFg&) = delete;

    // feature_dir contains nvngx_dlssg.dll (and usually nvngx.dll).
    // shared_device12 is the D3D12 device NGX was already initialized with by
    // DLSS SR. NGX has one global init per process; without sharing the device
    // the second Init fails with OutOfDate.
    bool Start(ID3D11Device* device, const std::wstring& feature_dir, int width,
               int height, int multiplier, std::string& detail,
               ID3D12Device* shared_device12 = nullptr);
    void Stop();

    // Uploads RGBA, evaluates the feature and waits for the GPU. Frames() is
    // valid until the next Process/Stop.
    bool Process(const std::uint8_t* rgba, int width, int height, bool reset,
                 std::string& detail);
    bool ProcessShared(HANDLE shared, int width, int height, bool reset,
                       std::string& detail);
    bool ProcessTexture(ID3D12Resource* color, int width, int height, bool reset,
                        std::string& detail);

    // Global screen-space motion in pixels (current minus previous). Video has
    // no engine motion vectors; a global estimate still lets the generator
    // interpolate pans instead of repeating the real frame.
    void SetGlobalMotion(float dx, float dy, bool reliable = true) {
        motion_x_ = dx;
        motion_y_ = dy;
        interpolation_ok_ = reliable;
    }
    bool interpolation_enabled() const { return interpolation_ok_; }

    const std::vector<Frame>& Frames() const { return frames_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // What the caller asked for, before the driver clamps the frame count. The
    // shell compares this value; returning the clamped one made it recreate the
    // feature every frame (5 fps) when a multiplier above the driver maximum
    // was selected.
    int requested_multiplier() const { return requested_multiplier_; }
    int actual_generated() const { return generated_; }
    bool available() const { return feature_ != nullptr; }
    bool HasOpticalFlow() const { return of_ != nullptr; }
    const std::string& status() const { return status_; }
    double last_ms() const { return last_ms_; }

private:
    struct Api;
    bool EnsureDevice(std::string& detail);
    bool EnsureCore(std::string& detail);
    bool CreateResources(std::string& detail);
    bool ClearGuides(std::string& detail);
    bool CreateFeature(std::string& detail);
    bool Evaluate(bool reset, std::string& detail);
    bool Submit(std::string& detail);
    bool Wait(std::string& detail);

    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;
    int multiplier_ = 2;
    int requested_multiplier_ = 2;
    int generated_ = 1;
    int max_generated_ = 1;
    double last_ms_ = 0.0;
    std::string status_;
    std::wstring feature_dir_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    ID3D12Device* shared_device12_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;
    UINT64 submitted_ = 0;
    HANDLE fence_event_ = nullptr;
    HANDLE feature_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> input_;
    Microsoft::WRL::ComPtr<ID3D12Resource> previous_input_;
    // DLSS SR rotates two shared output textures; reopen each handle once and
    // keep the D3D12 views instead of opening every frame.
    struct SharedInput {
        HANDLE handle = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    };
    std::vector<SharedInput> shared_inputs_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    std::uint8_t* upload_ptr_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_of_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> motion_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE motion_cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE motion_gpu_{};
    float motion_x_ = 0.0f;
    float motion_y_ = 0.0f;
    bool interpolation_ok_ = true;
    ID3D12Resource* motion_source_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> real_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> generated_tex_;
    std::vector<HANDLE> generated_handles_;
    HANDLE real_handle_ = nullptr;

    void* feature_ = nullptr;
    void* parameters_ = nullptr;
    std::vector<Frame> frames_;
    std::unique_ptr<Api> api_;
    bool input_ready_ = false;
    int input_slot_ = 0;
    int frames_fed_ = 0;
    std::uint64_t frame_id_ = 0;
    bool have_previous_ = false;
    bool of_failed_ = false;
    ID3D12Resource* eval_input_ = nullptr;
    std::unique_ptr<NvOf> of_;
};

}  // namespace brp::media
