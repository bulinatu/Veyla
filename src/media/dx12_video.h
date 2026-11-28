#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace brp {

// Pure Direct3D 12 video surface for the Watch window.
//
// The D3D11 Presenter stays with the shell and the legacy UI. This class is
// the stream path: a native DX12 swapchain, a DX12 queue and DX12 textures end
// to end, so DLSS/FSR/frame generation can run in-process without a worker
// process, an injected proxy or a second topmost window.
//
// Decoding may still be D3D11VA. A decoded frame arrives either as packed RGBA
// (uploaded once) or as an NT shared handle from the decoder or an AI stage.
class Dx12Video {
public:
    Dx12Video() = default;
    ~Dx12Video();

    Dx12Video(const Dx12Video&) = delete;
    Dx12Video& operator=(const Dx12Video&) = delete;

    bool Init(HWND hwnd, int width, int height, ID3D11Device* decode_device,
              std::string& detail);
    bool Resize(int width, int height);

    // Packed RGBA8 at `width` x `height`. The current stream path is SDR;
    // `pq` is accepted so the HDR hand-off can be added without changing the
    // call sites.
    bool DrawCpu(const std::uint8_t* rgba, int width, int height, bool pq);

    // A DX12 texture shared from another device (NT handle). The producer must
    // have transitioned it to COMMON and waited for its GPU work before the
    // handle is used.
    bool DrawShared(HANDLE shared, int width, int height, bool pq);

    // A texture owned by this device.
    bool DrawTexture(ID3D12Resource* texture, int width, int height);

    bool Present(bool vsync);
    bool WaitForFrame(DWORD timeout_ms);

    // Debug: copies the just-drawn backbuffer to a 32-bit BMP. Call between a
    // Draw* call and Present.
    bool CaptureToBmp(const char* path);

    bool ready() const { return swapchain_ != nullptr; }
    bool device_lost() const { return device_lost_; }
    bool has_frame() const { return has_frame_; }
    int width() const { return width_; }
    int height() const { return height_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    IDXGISwapChain3* swapchain() const { return swapchain_.Get(); }

    // Number of refreshes the display repeated while presents were pending.
    unsigned long long present_misses() const { return present_misses_; }
    UINT present_refresh_count() const { return last_refresh_; }
    bool present_stats_available() const { return have_stats_; }
    double present_lag_ms() const { return present_lag_ms_; }
    int present_queue() const { return present_queue_; }

    const std::string& status() const { return status_; }
    const std::string& shader_model() const { return shader_model_; }

private:
    bool CreateDevice(ID3D11Device* decode_device, std::string& detail);
    bool CreateSwapchain(std::string& detail);
    bool CreatePipeline(std::string& detail);
    bool CreateRtvHeap(std::string& detail);
    bool EnsureSource(int width, int height, std::string& detail);
    bool OpenShared(HANDLE handle, std::string& detail);
    void ReleaseShared();

    bool BeginFrame(bool clear);
    void DrawFullscreen(ID3D12Resource* source, int width, int height);
    bool EndFrame();
    void WaitGpu();
    void UpdatePresentStats();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool device_lost_ = false;
    bool has_frame_ = false;
    bool tearing_ = false;
    bool waitable_ = false;
    HANDLE frame_waitable_ = nullptr;
    std::string status_;
    std::string shader_model_;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;
    UINT64 submitted_value_ = 0;
    HANDLE fence_event_ = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backbuffers_[3];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_stride_ = 0;
    UINT buffer_count_ = 2;
    UINT swapchain_flags_ = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    UINT srv_stride_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> source_;
    int source_w_ = 0;
    int source_h_ = 0;
    bool source_drawn_ = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    std::uint8_t* upload_ptr_ = nullptr;

    // The real/generated frames and the SR rotation hand us a few stable
    // handles; keep their D3D12 views instead of reopening every frame.
    struct SharedSource {
        HANDLE handle = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    };
    std::vector<SharedSource> shared_sources_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shared_resource_;

    // Present statistics, same meaning as in Presenter.
    bool have_stats_ = false;
    DXGI_FRAME_STATISTICS last_stats_{};
    unsigned long long present_misses_ = 0;
    UINT last_refresh_ = 0;
    UINT64 present_qpc_ = 0;
    double refresh_period_ms_ = 0.0;
    double present_lag_ms_ = 0.0;
    int present_queue_ = 0;
    bool have_sync_ = false;
    UINT last_sync_refresh_ = 0;
    LARGE_INTEGER last_sync_qpc_{};
};

}  // namespace brp
