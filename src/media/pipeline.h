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
    // 0 = render directly at the output (window/display) size. Otherwise the
    // video chain upscales to this target and a final pass fits it to the
    // output. 2560x1440 / 3840x2160 enable 1440p/2160p rendering, including in
    // a smaller window and in screenshots.
    int render_width = 0;
    int render_height = 0;
    bool vsync = false;
    bool allow_tearing = true;
    bool hdr = false;
    bool source_pq = false;  // decoded source is PQ/BT.2020 (HDR10)
    bool source_final = false;  // source is already at the render target size
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
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Init(HWND hwnd, const PresenterConfig& cfg);

    // Recreates swapchain buffers. Safe to call on every WM_SIZE.
    bool Resize(int width, int height);

    // Builds and submits one frame. Does not block on the GPU.
    void Render(double time_seconds);

    // Presents the submitted frame and harvests completed GPU timings.
    void Present();

    // Frame-latency waitable (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT).
    // Waiting on it before building a frame retires the previous present and
    // gives the loop a vblank-aligned cadence instead of racing a software
    // timer against the display. Null when the platform/driver did not give us
    // one; callers then fall back to their own pacing.
    HANDLE frame_waitable() const { return frame_waitable_; }
    bool WaitForFrame(DWORD timeout_ms);

    // Frames that DXGI reported as presented but whose refresh count advanced
    // by more than the present count: i.e. the scanout repeated a frame.
    unsigned long long present_misses() const { return present_misses_; }
    bool present_stats_available() const { return have_present_stats_; }
    bool device_lost() const { return device_lost_; }
    // Refresh counter from the last Present. Consecutive new-source presents
    // differ by the exact number of vblanks the previous source frame stayed
    // on screen.
    UINT present_refresh_count() const { return last_present_refresh_; }

    void SetUpscaleMode(int mode);
    int upscale_mode() const { return cfg_.upscale_mode; }

    // 0,0 = render at the output size. 1920x1080 / 2560x1440 / 3840x2160 pick
    // an explicit render target; the result is fitted to the window/display.
    bool SetUpscaleTarget(int width, int height);
    int render_width() const {
        return cfg_.render_width > 0 ? cfg_.render_width : cfg_.output_width;
    }
    int render_height() const {
        return cfg_.render_height > 0 ? cfg_.render_height : cfg_.output_height;
    }

    // Accessors so the app can composite Direct2D UI over the same surface.
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGISwapChain1* swapchain() const { return swapchain_.Get(); }
    ID3D11Texture2D* backbuffer() const { return backbuffer_.Get(); }
    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }

    // Uploads a decoded RGBA frame as the source instead of the synthetic one.
    // `source_pq` marks an HDR10 PQ/BT.2020 source (pass-through when the
    // swapchain is HDR, tone-mapped when it is SDR).
    bool SetSourceFrame(const uint8_t* rgba, int width, int height, bool source_pq = false);

    // Uses an existing GPU texture view (frame generation output) as the source.
    // The caller keeps the view alive until the next SetSourceView/Render.
    void SetSourceView(ID3D11ShaderResourceView* view, int width, int height,
                       bool source_pq = false);

    // Marks the source as already upscaled to the render target (an earlier
    // stage pass then copies without running the selected upscaler/sharpen).
    void SetSourceFinal(bool final) { cfg_.source_final = final; }

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
    bool CreateTargetViews();
    bool CreateSourceTexture();
    bool CreateShaders();
    bool CreateTimingQueries();

    void BeginTiming();
    void EndTiming();
    void CollectTiming();

    HWND hwnd_ = nullptr;
    PresenterConfig cfg_{};
    bool tearing_ = false;
    bool external_source_ = false;
    bool CreateSwapchainForFormat();
    UINT swapchain_flags_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapchain2_;
    HANDLE frame_waitable_ = nullptr;
    bool waitable_ = false;
    DXGI_FRAME_STATISTICS last_present_stats_{};
    bool have_present_stats_ = false;
    bool present_stats_logged_ = false;
    unsigned long long present_misses_ = 0;
    UINT last_present_refresh_ = 0;
    bool device_lost_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    // Offscreen render target for an explicit 1080p/1440p/2160p upscale target.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> target_srv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_override_;
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

    // The HUD asks for a summary every frame; sorting the timing history that
    // often is pointless, so it is rebuilt a few times a second.
    mutable std::string summary_cache_;
    mutable std::chrono::steady_clock::time_point summary_at_{};
    mutable unsigned long long summary_frames_ = 0;
};

}  // namespace brp
