#pragma once

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace brp::media {

// NVIDIA Video Effects (Maxine) Video Frame Generation.
//
// This is the video-domain frame generator used by Magpie's "NVIDIA VFG"
// effect: it takes two consecutive 8-bit SDR frames and synthesizes frames at
// t = i / multiplier. It needs the proprietary NVIDIA VFX runtime (Ada or
// Blackwell GPU), which is loaded at runtime and is not distributed with this
// repository. See docs/16-frame-generation.md.
class NvVfx {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        bool generated = false;
    };

    NvVfx();
    ~NvVfx();

    NvVfx(const NvVfx&) = delete;
    NvVfx& operator=(const NvVfx&) = delete;

    // Loads nvcuda + the VFX runtime from `directory`. Safe to call once.
    bool LoadRuntime(const std::wstring& directory, std::string& detail);

    // Creates the CUDA context for the D3D11 adapter, the effect and its images.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width,
                    int height, int model, std::string& detail);

    // Uploads one decoded RGBA frame and returns the presentation sequence for
    // this source frame: (multiplier - 1) generated frames followed by the real
    // frame. The first call returns only the real frame.
    bool PushFrame(const uint8_t* rgba, int width, int height, int multiplier,
                   std::vector<Frame>& sequence, std::string& detail);

    bool runtime_loaded() const { return runtime_loaded_; }
    const std::string& status() const { return status_; }
    double last_generate_ms() const { return last_generate_ms_; }

    void Shutdown();

private:
    struct Image;

    bool LoadModule(const std::wstring& directory, const wchar_t* name, HMODULE& module,
                    std::string& detail);
    bool ResolveApi(std::string& detail);
    bool CreateCudaContext(ID3D11Device* device, std::string& detail);
    bool CreateEffect(int width, int height, int model, std::string& detail);
    void DestroyEffect();

    bool ImportTexture(ID3D11Texture2D* texture, Image& destination, std::string& detail);
    bool ExportTexture(Image& source, ID3D11Texture2D* texture, std::string& detail);
    bool CreateInputTextures(int width, int height, std::string& detail);
    bool CreateOutputTextures(int width, int height, std::string& detail);
    bool SetImages(Image& previous, Image& current, Image& output, std::string& detail);
    std::string ErrorText(int status) const;

    bool runtime_loaded_ = false;
    bool cuda_ready_ = false;
    bool has_previous_ = false;
    int width_ = 0;
    int height_ = 0;
    int model_ = 1;
    int pixel_format_ = 6;  // NVCV_RGBA; 7 = NVCV_BGRA fallback
    int in_index_ = 0;
    int out_index_ = 0;
    int failures_ = 0;
    void* effect_ = nullptr;
    void* stream_ = nullptr;
    void* cuda_context_ = nullptr;
    int cuda_device_ = -1;
    double last_generate_ms_ = 0.0;
    std::string status_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    struct LinearImage;
    std::vector<std::unique_ptr<LinearImage>> linear_;  // previous, current, output
    int previous_index_ = 0;
    int current_index_ = 1;

    struct TextureSlot;
    std::vector<std::unique_ptr<TextureSlot>> inputs_;
    std::vector<std::unique_ptr<TextureSlot>> outputs_;
    std::unique_ptr<LinearImage> transfer_temp_;

    // Dynamically resolved API.
    HMODULE cuda_module_ = nullptr;
    HMODULE ngx_runtime_ = nullptr;
    HMODULE cv_image_ = nullptr;
    HMODULE video_effects_ = nullptr;
    HMODULE vfg_ = nullptr;
    std::vector<HMODULE> npp_modules_;

    struct Api;
    std::unique_ptr<Api> api_;
};

}  // namespace brp::media
