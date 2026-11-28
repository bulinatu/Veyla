#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brp::media {

// AMD FSR Upscaling through the signed AMD FSR SDK 2.4 loader (D3D12).
//
// FSR 4.1.2 runs on RDNA 2/3/4 and supported Ryzen APUs; on every other GPU
// the same API selects FSR 3.1.6. The caller may pin either family explicitly
// so the UI can offer "FSR 4" and "FSR 3" separately.
//
// A video stream has no camera guides. The default is zero depth + zero motion
// vectors with a per-frame reset, which is a stable spatial ML upscale. When
// real motion vectors are available (NVOFA) the reset can be turned off for
// the temporal path.
class AmdFsr {
public:
    enum class Variant {
        Auto = 0,  // newest available (4.1.2 on AMD, 3.1.6 elsewhere)
        Fsr4 = 1,  // pin 4.x; falls back to Auto if creation fails
        Fsr3 = 2,  // pin 3.1.6
    };

    AmdFsr();
    ~AmdFsr();

    AmdFsr(const AmdFsr&) = delete;
    AmdFsr& operator=(const AmdFsr&) = delete;

    // directory contains amd_fidelityfx_loader_dx12.dll and
    // amd_fidelityfx_upscaler_dx12.dll.
    bool LoadRuntime(const std::wstring& directory, std::string& detail);

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                    int in_height, int out_width, int out_height, Variant variant,
                    bool hdr_input, std::string& detail);
    void Shutdown();

    // Uploads RGBA8, dispatches FSR and waits for the output. The output is a
    // shared NT handle; the DX12 surface opens it and samples it.
    bool Process(const std::uint8_t* rgba, int width, int height, float frame_time_ms,
                 std::string& detail);

    HANDLE SharedHandle() const { return output_handle_; }
    int in_width() const { return in_width_; }
    int in_height() const { return in_height_; }
    int out_width() const { return out_width_; }
    int out_height() const { return out_height_; }
    bool ready() const { return context_ != nullptr; }
    bool runtime_loaded() const { return runtime_loaded_; }
    const std::string& provider() const { return provider_; }
    const std::string& status() const { return status_; }
    double last_ms() const { return last_ms_; }
    void SetResetEachFrame(bool reset) { reset_each_frame_ = reset; }

private:
    struct Api;
    bool EnsureDevice(std::string& detail);
    bool QueryVersions();
    bool CreateContext(Variant variant, std::string& detail);
    bool CreateResources(std::string& detail);
    bool ClearGuides(std::string& detail);
    bool Submit(std::string& detail);
    bool Wait(std::string& detail);

    bool runtime_loaded_ = false;
    bool device_ready_ = false;
    int in_width_ = 0;
    int in_height_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
    bool hdr_input_ = false;
    bool reset_each_frame_ = true;
    double last_ms_ = 0.0;
    std::string provider_;
    std::string status_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;
    UINT64 submitted_ = 0;
    HANDLE fence_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> input_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    std::uint8_t* upload_ptr_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_;
    Microsoft::WRL::ComPtr<ID3D12Resource> output_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> guide_heap_;
    HANDLE output_handle_ = nullptr;

    void* context_handle_ = nullptr;
    std::vector<std::uint64_t> version_ids_;
    std::vector<std::string> version_names_;
    std::string chosen_version_;

    std::unique_ptr<Api> api_;
};

}  // namespace brp::media
