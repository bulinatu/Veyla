#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brp::media {

// NVIDIA DLSS Super Resolution on D3D12, using Magpie's captured-colour
// contract: the decoded 1080p frame is the colour input, depth and motion
// vectors are zero, and history is reset every frame so a 2D video stream does
// not ghost. The feature writes directly at the render target size.
class NvDlssSr {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    NvDlssSr();
    ~NvDlssSr();

    NvDlssSr(const NvDlssSr&) = delete;
    NvDlssSr& operator=(const NvDlssSr&) = delete;

    // Loads nvngx_dlss.dll and the NGX D3D12 core. Safe to call once.
    bool LoadRuntime(const std::wstring& directory, std::string& detail);

    // NVIDIA quality modes, in NGX enum order.
    enum PerfQuality {
        MaxPerf = 0,
        Balanced = 1,
        MaxQuality = 2,
        UltraPerformance = 3,
        UltraQuality = 4,
        Dlaa = 5,
    };

    // Creates the D3D12 feature and its shared textures. Re-initializes when
    // the geometry, preset or quality changes.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                    int in_height, int out_width, int out_height, int preset, int quality,
                    std::string& detail);

    // Copies the last SR output back to CPU RGBA (target resolution). Used by
    // the NVIDIA pipeline order, where NR runs on the upscaled frame.
    bool Readback(std::vector<uint8_t>& out, std::string& detail);
    // Reads the previous completed output without waiting. Used to feed the
    // frame-generation worker while the next SR frame is in flight.
    bool ReadbackLast(std::vector<uint8_t>& out, std::string& detail);

    bool Process(const uint8_t* rgba, int width, int height, Frame& out, std::string& detail);

    bool runtime_loaded() const { return runtime_loaded_; }
    bool ready() const { return feature_ != nullptr; }
    double last_ms() const { return last_ms_; }
    int out_width() const { return out_width_; }
    int out_height() const { return out_height_; }
    const std::string& status() const { return status_; }

    void Shutdown();

private:
    struct Api;

    bool EnsureDevice(std::string& detail);
    bool EnsureCore(std::string& detail);
    bool CreateTextures(int in_width, int in_height, int out_width, int out_height,
                        std::string& detail);
    bool ClearGuides(std::string& detail);
    bool CreateFeature(int preset, int quality, std::string& detail);
    bool Submit(std::string& detail);
    bool WaitPending(std::string& detail);
    bool ReadbackSlot(int slot, std::vector<uint8_t>& out, std::string& detail);
    std::string ErrorText(int result) const;

    bool runtime_loaded_ = false;
    bool device_ready_ = false;
    bool core_ready_ = false;
    bool initialized_ = false;
    void* feature_ = nullptr;
    void* parameters_ = nullptr;
    int in_width_ = 0;
    int in_height_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
    int preset_ = -1;
    int quality_ = -1;
    double last_ms_ = 0.0;
    std::string status_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion12_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth12_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> guide_heap_;
    uint64_t fence_value_ = 0;
    HANDLE wait_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> input11_;
    Microsoft::WRL::ComPtr<ID3D12Resource> input12_;
    static constexpr int kOutputSlots = 2;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output11_[kOutputSlots];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_[kOutputSlots];
    Microsoft::WRL::ComPtr<ID3D12Resource> output12_[kOutputSlots];
    int output_slot_ = 0;
    int last_output_slot_ = 0;
    int pending_slot_ = 0;
    uint64_t pending_value_ = 0;
    bool have_pending_ = false;
    bool have_output_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_staging_;

    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;

    std::unique_ptr<Api> api_;
};

}  // namespace brp::media
