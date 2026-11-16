#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <string>
#include <vector>

namespace brp {

struct PresenterConfig {
    int output_width = 2560;
    int output_height = 1440;
    int source_width = 1920;
    int source_height = 1080;
    bool vsync = false;
    bool allow_tearing = true;
    int upscale_mode = 2;  // 0 bilinear, 1 catmull-rom, 2 catmull-rom + sharpen
};

// Owns the D3D11 device, the flip-model swapchain, and the post-process chain.
//
// Frame shape today:  synthetic source -> upscale -> present
// Frame shape in Phase 2:  RTP -> hw decode -> color -> upscale -> [fg] -> present
//
// The point of this class is that the *presentation* contract is fixed now:
// one texture in, one presented frame out, per-frame GPU timing always on.
class Presenter {
public:
    Presenter() = default;
    ~Presenter() = default;

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Init(HWND hwnd, const PresenterConfig& cfg);

    // Recreates swapchain buffers. Safe to call on every WM_SIZE.
    bool Resize(int width, int height);

    // Builds and submits one frame. Does not block on the GPU.
    void Render(double time_seconds);

    // Presents the submitted frame and harvests completed GPU timings.
    void Present();

    void SetUpscaleMode(int mode);
    int upscale_mode() const { return cfg_.upscale_mode; }

    // Accessors so the app can composite Direct2D UI over the same surface.
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGISwapChain1* swapchain() const { return swapchain_.Get(); }
    ID3D11Texture2D* backbuffer() const { return backbuffer_.Get(); }
    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }

    void Clear(const float rgba[4]);
    void PresentNow(bool vsync);

    // Copies the current backbuffer to a 32-bit BMP. Must be called between
    // Render() and Present(): the backbuffer is only readable there.
    bool CaptureToBmp(const char* path);

    int output_width() const { return cfg_.output_width; }
    int output_height() const { return cfg_.output_height; }

    std::string Summary() const;
    void PrintFinalSummary() const;

private:
    struct TimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool active = false;
    };

    bool CreateSwapchain();
    bool CreateBackbufferViews();
    bool CreateSourceTexture();
    bool CreateShaders();
    bool CreateTimingQueries();

    void BeginTiming();
    void EndTiming();
    void CollectTiming();

    HWND hwnd_ = nullptr;
    PresenterConfig cfg_{};
    bool tearing_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> source_uav_;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> gen_cs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cs_cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ps_cb_;

    static constexpr int kTimingSlots = 6;
    TimingSlot slots_[kTimingSlots];
    int next_slot_ = 0;
    int current_slot_ = -1;

    std::vector<double> gpu_ms_;
    std::vector<double> cpu_ms_;
    std::chrono::steady_clock::time_point start_time_{};
    unsigned long long frames_ = 0;
};

}  // namespace brp
