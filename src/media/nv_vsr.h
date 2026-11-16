#pragma once

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brp::media {

// NVIDIA Video Effects Video Super Resolution (RTX VSR).
//
// Neural upscaling/denoise/deblur for 8-bit RGBA (RGB10A2 is possible later).
// It uses the same proprietary NVIDIA VFX runtime as frame generation, which is
// loaded at runtime and never redistributed by this repository.
//
// Quality levels (NvVFX "QualityLevel"):
//   0 bicubic (non-neural), 1..4 low/medium/high/ultra,
//   8..11 denoise, 12..15 deblur, 16..19 high bitrate,
//   21 / 23 streaming medium/ultra.
// Denoise/deblur cannot change resolution; upscaling modes can.
class NvVsr {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    NvVsr();
    ~NvVsr();

    NvVsr(const NvVsr&) = delete;
    NvVsr& operator=(const NvVsr&) = delete;

    // Loads nvcuda + the VFX/VSR runtime from `directory`. Safe to call once.
    bool LoadRuntime(const std::wstring& directory, std::string& detail);

    // Creates the CUDA context, the effect and its images. Re-initializes when
    // the geometry or the quality changes.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                    int in_height, int out_width, int out_height, int quality,
                    float strength, std::string& detail);

    // Uploads one decoded RGBA frame, runs VSR and leaves the result in `out`.
    bool Process(const uint8_t* rgba, int width, int height, Frame& out, std::string& detail);

    bool runtime_loaded() const { return runtime_loaded_; }
    bool ready() const { return effect_ != nullptr; }
    double last_ms() const { return last_ms_; }
    const std::string& status() const { return status_; }
    int out_width() const { return out_width_; }
    int out_height() const { return out_height_; }

    void Shutdown();

private:
    struct Image;
    struct LinearImage;
    struct TextureSlot;
    struct Api;

    bool LoadModule(const std::wstring& directory, const wchar_t* name, HMODULE& module,
                    std::string& detail);
    bool ResolveApi(std::string& detail);
    bool CreateCudaContext(ID3D11Device* device, std::string& detail);
    bool CreateEffect(int in_width, int in_height, int out_width, int out_height, int quality,
                      float strength, std::string& detail);
    void DestroyEffect();
    bool CreateTextures(int in_width, int in_height, int out_width, int out_height,
                        std::string& detail);
    bool ImportInput(const uint8_t* rgba, int width, std::string& detail);
    bool ExportOutput(std::string& detail);
    std::string ErrorText(int status) const;

    bool runtime_loaded_ = false;
    bool cuda_ready_ = false;
    int pixel_format_ = 6;  // NVCV_RGBA; 7 = NVCV_BGRA fallback
    void* effect_ = nullptr;
    void* stream_ = nullptr;
    void* cuda_context_ = nullptr;
    int cuda_device_ = -1;
    int in_width_ = 0;
    int in_height_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
    int quality_ = -1;
    double last_ms_ = 0.0;
    std::string status_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::unique_ptr<LinearImage> input_image_;
    std::unique_ptr<LinearImage> output_image_;
    std::unique_ptr<LinearImage> transfer_temp_;
    std::unique_ptr<TextureSlot> input_texture_;
    std::unique_ptr<TextureSlot> output_texture_;

    // Dynamically resolved API.
    HMODULE cuda_module_ = nullptr;
    HMODULE cudart_ = nullptr;
    HMODULE ngx_runtime_ = nullptr;
    HMODULE cv_image_ = nullptr;
    HMODULE video_effects_ = nullptr;
    HMODULE vsr_ = nullptr;
    HMODULE ngx_vsr_ = nullptr;
    std::vector<HMODULE> npp_modules_;

    std::unique_ptr<Api> api_;
};

}  // namespace brp::media
