#include "media/nv_vsr.h"

#include <dxgi.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace brp::media {
namespace {

using NvCVStatus = int;

// Opaque storage for NvCVImage. The SDK owns the layout; we only pass the
// pointer back to its API. 512 bytes is well above the real descriptor size.
constexpr int kImageStorage = 512;

struct NvImage {
    alignas(64) unsigned char bytes[kImageStorage]{};
};

using CreateEffectFn = NvCVStatus (*)(const char*, void**);
using DestroyEffectFn = NvCVStatus (*)(void*);
using SetU32Fn = NvCVStatus (*)(void*, const char*, unsigned);
using SetF32Fn = NvCVStatus (*)(void*, const char*, float);
using SetImageFn = NvCVStatus (*)(void*, const char*, void*);
using SetCudaStreamFn = NvCVStatus (*)(void*, const char*, void*);
using LoadFn = NvCVStatus (*)(void*);
using RunFn = NvCVStatus (*)(void*, int);
using GetVersionFn = NvCVStatus (*)(unsigned*);
using CudaStreamCreateFn = NvCVStatus (*)(void**);
using CudaStreamDestroyFn = NvCVStatus (*)(void*);
using CudaStreamSyncFn = NvCVStatus (*)(void*);
using ErrorStringFn = const char* (*)(int);

using ImageAllocFn = NvCVStatus (*)(void*, unsigned, unsigned, int, int, int, int, unsigned);
using ImageDeallocFn = NvCVStatus (*)(void*);
using ImageInitFromD3DFn = NvCVStatus (*)(void*, ID3D11Texture2D*);
using ImageMapFn = NvCVStatus (*)(void*, void*);
using ImageUnmapFn = NvCVStatus (*)(void*, void*);
using ImageTransferFn = NvCVStatus (*)(void*, void*, float, void*, void*);

using CuInitFn = int (*)(unsigned);
using CuDeviceGetFn = int (*)(int*, int);
using CuD3D11GetDeviceFn = int (*)(int*, IDXGIAdapter*);
using CuPrimaryCtxRetainFn = int (*)(void**, int);
using CuPrimaryCtxReleaseFn = int (*)(int);
using CuCtxPushFn = int (*)(void*);
using CuCtxPopFn = int (*)(void**);
using CuAttributeFn = int (*)(int*, int, int);
using CudaSetDeviceFn = int (*)(int);

constexpr int kNvcvU8 = 1;
constexpr int kNvcvInterleaved = 0;
constexpr int kNvcvGpu = 2;

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

}  // namespace

struct NvVsr::Image {
    NvImage image;
    bool allocated = false;
};

struct NvVsr::LinearImage {
    Image image;
};

struct NvVsr::TextureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Image wrapper;
    bool wrapper_ready = false;
};

struct NvVsr::Api {
    CreateEffectFn create_effect = nullptr;
    DestroyEffectFn destroy_effect = nullptr;
    SetU32Fn set_u32 = nullptr;
    SetF32Fn set_f32 = nullptr;
    SetImageFn set_image = nullptr;
    SetCudaStreamFn set_cuda_stream = nullptr;
    LoadFn load = nullptr;
    RunFn run = nullptr;
    GetVersionFn get_version = nullptr;
    CudaStreamCreateFn stream_create = nullptr;
    CudaStreamDestroyFn stream_destroy = nullptr;
    CudaStreamSyncFn stream_sync = nullptr;
    ErrorStringFn error_string = nullptr;

    ImageAllocFn image_alloc = nullptr;
    ImageDeallocFn image_dealloc = nullptr;
    ImageInitFromD3DFn image_init_d3d = nullptr;
    ImageMapFn image_map = nullptr;
    ImageUnmapFn image_unmap = nullptr;
    ImageTransferFn image_transfer = nullptr;

    CuInitFn cu_init = nullptr;
    CuDeviceGetFn cu_device_get = nullptr;
    CuD3D11GetDeviceFn cu_d3d11_device = nullptr;
    CuPrimaryCtxRetainFn cu_ctx_retain = nullptr;
    CuPrimaryCtxReleaseFn cu_ctx_release = nullptr;
    CuCtxPushFn cu_ctx_push = nullptr;
    CuCtxPopFn cu_ctx_pop = nullptr;
    CuAttributeFn cu_attribute = nullptr;
    CudaSetDeviceFn cuda_set_device = nullptr;
};

NvVsr::NvVsr() = default;

NvVsr::~NvVsr() {
    Shutdown();
}

bool NvVsr::LoadRuntime(const std::wstring& directory, std::string& detail) {
    if (runtime_loaded_) {
        return true;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::absolute(directory, ec);
    if (ec || !fs::exists(dir)) {
        detail = "NVIDIA VFX runtime directory not found: " + dir.string();
        status_ = detail;
        return false;
    }

    ::SetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", L"USE_APP_PATH");
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(dir.c_str());

    const std::wstring dir_text = dir.wstring();
    auto load = [&](const wchar_t* name, HMODULE& module, std::string& error) {
        return LoadModule(dir_text, name, module, error);
    };

    // nvcuda is a system driver DLL; everything else is app-local.
    cuda_module_ = ::LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda_module_) {
        detail = "nvcuda.dll not found (NVIDIA driver required)";
        status_ = detail;
        return false;
    }

    std::string error;
    const wchar_t* required[] = {
        L"cudart64_12.dll", L"nvngxruntime.dll", L"NVCVImage.dll", L"NVVideoEffects.dll",
        L"nvngx_vsr.dll",   L"nvVFXVideoSuperRes.dll",
    };
    HMODULE* targets[] = {&cudart_, &ngx_runtime_, &cv_image_, &video_effects_, &ngx_vsr_, &vsr_};
    for (int i = 0; i < 6; ++i) {
        HMODULE module = nullptr;
        if (!load(required[i], module, error)) {
            detail = error;
            status_ = detail;
            return false;
        }
        *targets[i] = module;
    }

    // NPP is used by the models. Missing ones are tolerated; the error from the
    // effect load is clearer than a loader failure.
    const wchar_t* npp[] = {
        L"nppc64_12.dll",   L"nppial64_12.dll", L"nppicc64_12.dll", L"nppidei64_12.dll",
        L"nppif64_12.dll",  L"nppig64_12.dll",  L"nppim64_12.dll",  L"nppist64_12.dll",
        L"nppitc64_12.dll",
    };
    for (const wchar_t* name : npp) {
        HMODULE module = nullptr;
        std::string ignored;
        if (load(name, module, ignored) && module) {
            npp_modules_.push_back(module);
        }
    }

    if (!ResolveApi(detail)) {
        status_ = detail;
        return false;
    }
    unsigned version = 0;
    if (api_->get_version && api_->get_version(&version) == 0) {
        std::fprintf(stderr, "[vsr] runtime version 0x%08X\n", version);
    }
    runtime_loaded_ = true;
    status_ = "runtime loaded";
    return true;
}

bool NvVsr::LoadModule(const std::wstring& directory, const wchar_t* name, HMODULE& module,
                       std::string& detail) {
    if (module) {
        return true;
    }
    module = ::GetModuleHandleW(name);
    if (module) {
        return true;
    }
    const std::wstring path = directory + L"\\" + name;
    module = ::LoadLibraryExW(path.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                  LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
        char text[512];
        std::snprintf(text, sizeof(text), "cannot load %ls (error %lu)", name,
                      static_cast<unsigned long>(::GetLastError()));
        detail = text;
        return false;
    }
    return true;
}

bool NvVsr::ResolveApi(std::string& detail) {
    api_ = std::make_unique<Api>();
    auto from = [](HMODULE module, const char* name) -> void* {
        return module ? reinterpret_cast<void*>(::GetProcAddress(module, name)) : nullptr;
    };
    auto need = [&](HMODULE module, const char* name, auto& target) {
        target = reinterpret_cast<std::decay_t<decltype(target)>>(from(module, name));
        return target != nullptr;
    };

    bool ok = true;
    ok &= need(video_effects_, "NvVFX_CreateEffect", api_->create_effect);
    ok &= need(video_effects_, "NvVFX_DestroyEffect", api_->destroy_effect);
    ok &= need(video_effects_, "NvVFX_SetU32", api_->set_u32);
    ok &= need(video_effects_, "NvVFX_SetF32", api_->set_f32);
    ok &= need(video_effects_, "NvVFX_SetImage", api_->set_image);
    ok &= need(video_effects_, "NvVFX_SetCudaStream", api_->set_cuda_stream);
    ok &= need(video_effects_, "NvVFX_Load", api_->load);
    ok &= need(video_effects_, "NvVFX_Run", api_->run);
    ok &= need(video_effects_, "NvVFX_GetVersion", api_->get_version);
    ok &= need(video_effects_, "NvVFX_CudaStreamCreate", api_->stream_create);
    ok &= need(video_effects_, "NvVFX_CudaStreamDestroy", api_->stream_destroy);
    ok &= need(video_effects_, "NvVFX_CudaStreamSynchronize", api_->stream_sync);
    if (!need(video_effects_, "NvCV_GetErrorStringFromCode", api_->error_string)) {
        need(cv_image_, "NvCV_GetErrorStringFromCode", api_->error_string);
    }

    ok &= need(cv_image_, "NvCVImage_Alloc", api_->image_alloc);
    ok &= need(cv_image_, "NvCVImage_Dealloc", api_->image_dealloc);
    ok &= need(cv_image_, "NvCVImage_InitFromD3D11Texture", api_->image_init_d3d);
    ok &= need(cv_image_, "NvCVImage_MapResource", api_->image_map);
    ok &= need(cv_image_, "NvCVImage_UnmapResource", api_->image_unmap);
    ok &= need(cv_image_, "NvCVImage_Transfer", api_->image_transfer);

    ok &= need(cuda_module_, "cuInit", api_->cu_init);
    ok &= need(cuda_module_, "cuDeviceGet", api_->cu_device_get);
    ok &= need(cuda_module_, "cuD3D11GetDevice", api_->cu_d3d11_device);
    ok &= need(cuda_module_, "cuDevicePrimaryCtxRetain", api_->cu_ctx_retain);
    ok &= need(cuda_module_, "cuDevicePrimaryCtxRelease_v2", api_->cu_ctx_release);
    ok &= need(cuda_module_, "cuCtxPushCurrent_v2", api_->cu_ctx_push);
    ok &= need(cuda_module_, "cuCtxPopCurrent_v2", api_->cu_ctx_pop);
    ok &= need(cuda_module_, "cuDeviceGetAttribute", api_->cu_attribute);
    if (cudart_) {
        need(cudart_, "cudaSetDevice", api_->cuda_set_device);
    }

    if (!ok) {
        detail = "NVIDIA VFX VSR runtime is missing required exports";
        return false;
    }
    return true;
}

bool NvVsr::CreateCudaContext(ID3D11Device* device, std::string& detail) {
    if (cuda_ready_) {
        return true;
    }
    if (!api_ || !api_->cu_init || !device) {
        detail = "NVIDIA VFX VSR API not loaded";
        return false;
    }
    if (api_->cu_init(0) != 0) {
        detail = "cuInit failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (dxgi_device) {
        dxgi_device->GetAdapter(&adapter);
    }

    int cuda_device = 0;
    int status = 1;
    if (adapter) {
        status = api_->cu_d3d11_device(&cuda_device, adapter.Get());
    }
    if (status != 0) {
        if (api_->cu_device_get(&cuda_device, 0) != 0) {
            detail = "CUDA device selection failed";
            return false;
        }
        std::fprintf(stderr, "[vsr] cuD3D11GetDevice failed; using CUDA device 0\n");
    }

    int major = 0;
    int minor = 0;
    api_->cu_attribute(&major, 75, cuda_device);
    api_->cu_attribute(&minor, 76, cuda_device);
    if (major < 7) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "RTX VSR needs a Turing or newer NVIDIA GPU (found SM %d.%d)", major,
                      minor);
        detail = text;
        return false;
    }

    if (api_->cu_ctx_retain(&cuda_context_, cuda_device) != 0 || !cuda_context_) {
        detail = "cuDevicePrimaryCtxRetain failed";
        return false;
    }
    if (api_->cuda_set_device) {
        api_->cuda_set_device(cuda_device);
    }
    cuda_device_ = cuda_device;
    cuda_ready_ = true;
    std::fprintf(stderr, "[vsr] CUDA device %d, SM %d.%d\n", cuda_device, major, minor);
    return true;
}

bool NvVsr::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                       int in_height, int out_width, int out_height, int quality,
                       float strength, std::string& detail) {
    if (!runtime_loaded_ || !device || !context || in_width <= 0 || in_height <= 0 ||
        out_width <= 0 || out_height <= 0) {
        detail = "VSR is not initialized";
        return false;
    }
    if (effect_ && in_width == in_width_ && in_height == in_height_ &&
        out_width == out_width_ && out_height == out_height_ && quality == quality_) {
        return true;
    }
    Shutdown();
    device_ = device;
    context_ = context;

    if (!CreateCudaContext(device, detail)) {
        return false;
    }
    if (api_->cu_ctx_push(cuda_context_) != 0) {
        detail = "cuCtxPushCurrent failed";
        return false;
    }
    struct PopScope {
        Api* api;
        ~PopScope() {
            void* popped = nullptr;
            if (api && api->cu_ctx_pop) {
                api->cu_ctx_pop(&popped);
            }
        }
    } pop{api_.get()};

    if (api_->stream_create(&stream_) != 0 || !stream_) {
        detail = "NvVFX_CudaStreamCreate failed";
        return false;
    }

    auto alloc = [&](std::unique_ptr<LinearImage>& image, int width, int height) {
        image = std::make_unique<LinearImage>();
        int status = api_->image_alloc(&image->image.image, static_cast<unsigned>(width),
                                       static_cast<unsigned>(height), pixel_format_, kNvcvU8,
                                       kNvcvInterleaved, kNvcvGpu, 32);
        if (status != 0) {
            // Older VFX builds used different enum values; try BGRA once.
            pixel_format_ = 7;
            status = api_->image_alloc(&image->image.image, static_cast<unsigned>(width),
                                       static_cast<unsigned>(height), pixel_format_, kNvcvU8,
                                       kNvcvInterleaved, kNvcvGpu, 32);
        }
        if (status != 0) {
            detail = "NvCVImage_Alloc failed: " + ErrorText(status);
            return false;
        }
        image->image.allocated = true;
        return true;
    };
    if (!alloc(input_image_, in_width, in_height) || !alloc(output_image_, out_width, out_height)) {
        return false;
    }
    transfer_temp_ = std::make_unique<LinearImage>();

    if (!CreateTextures(in_width, in_height, out_width, out_height, detail) ||
        !CreateEffect(in_width, in_height, out_width, out_height, quality, strength, detail)) {
        return false;
    }

    in_width_ = in_width;
    in_height_ = in_height;
    out_width_ = out_width;
    out_height_ = out_height;
    quality_ = quality;
    std::fprintf(stderr,
                 "[vsr] initialized %dx%d -> %dx%d, quality %d, strength %.2f, format %d\n",
                 in_width_, in_height_, out_width_, out_height_, quality, strength,
                 pixel_format_);
    return true;
}

bool NvVsr::CreateTextures(int in_width, int in_height, int out_width, int out_height,
                           std::string& detail) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    input_texture_ = std::make_unique<TextureSlot>();
    desc.Width = static_cast<UINT>(in_width);
    desc.Height = static_cast<UINT>(in_height);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &input_texture_->texture))) {
        detail = "VSR input texture creation failed";
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(input_texture_->texture.Get(), nullptr,
                                                 &input_texture_->srv))) {
        detail = "VSR input SRV creation failed";
        return false;
    }

    output_texture_ = std::make_unique<TextureSlot>();
    desc.Width = static_cast<UINT>(out_width);
    desc.Height = static_cast<UINT>(out_height);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &output_texture_->texture))) {
        detail = "VSR output texture creation failed";
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(output_texture_->texture.Get(), nullptr,
                                                 &output_texture_->srv))) {
        detail = "VSR output SRV creation failed";
        return false;
    }
    return true;
}

bool NvVsr::CreateEffect(int in_width, int in_height, int out_width, int out_height, int quality,
                         float strength, std::string& detail) {
    (void)in_width;
    (void)in_height;
    (void)out_width;
    (void)out_height;

    int status = api_->create_effect("VideoSuperRes", &effect_);
    if (status != 0 || !effect_) {
        status = api_->create_effect("Video Super Resolution", &effect_);
    }
    if (status != 0 || !effect_) {
        detail = "NvVFX_CreateEffect(VideoSuperRes) failed: " + ErrorText(status);
        return false;
    }

    status = api_->set_cuda_stream(effect_, "CudaStream", stream_);
    if (status != 0) {
        detail = "VSR CudaStream failed: " + ErrorText(status);
        return false;
    }
    if (api_->set_u32(effect_, "BatchSize", 1) != 0 ||
        api_->set_u32(effect_, "ImageEncodingMode", 0) != 0) {
        detail = "VSR parameter setup failed";
        return false;
    }
    if (api_->set_u32(effect_, "QualityLevel", static_cast<unsigned>(quality)) != 0) {
        detail = "VSR QualityLevel failed";
        return false;
    }
    if (api_->set_f32(effect_, "Strength", strength) != 0) {
        detail = "VSR Strength failed";
        return false;
    }

    status = api_->set_image(effect_, "SrcImage0", &input_image_->image.image);
    if (status == 0) {
        status = api_->set_image(effect_, "DstImage0", &output_image_->image.image);
    }
    if (status != 0) {
        detail = "VSR image binding failed: " + ErrorText(status);
        return false;
    }

    status = api_->load(effect_);
    if (status != 0) {
        detail = "NvVFX_Load failed: " + ErrorText(status);
        return false;
    }
    return true;
}

void NvVsr::DestroyEffect() {
    if (!effect_) {
        return;
    }
    if (api_ && api_->cu_ctx_push && cuda_context_) {
        api_->cu_ctx_push(cuda_context_);
    }
    if (api_ && api_->destroy_effect) {
        api_->destroy_effect(effect_);
    }
    if (api_ && api_->cu_ctx_pop) {
        void* popped = nullptr;
        api_->cu_ctx_pop(&popped);
    }
    effect_ = nullptr;
}

bool NvVsr::ImportInput(const uint8_t* rgba, int width, std::string& detail) {
    if (!input_texture_->wrapper_ready) {
        std::memset(&input_texture_->wrapper.image, 0, sizeof(input_texture_->wrapper.image));
        const int init = api_->image_init_d3d(&input_texture_->wrapper.image,
                                              input_texture_->texture.Get());
        if (init != 0) {
            detail = "NvCVImage_InitFromD3D11Texture(input) failed: " + ErrorText(init);
            return false;
        }
        input_texture_->wrapper_ready = true;
    }

    context_->UpdateSubresource(input_texture_->texture.Get(), 0, nullptr, rgba,
                                static_cast<UINT>(width) * 4u, 0);
    context_->Flush();

    int status = api_->image_map(&input_texture_->wrapper.image, stream_);
    if (status != 0) {
        detail = "VSR input map failed: " + ErrorText(status);
        return false;
    }
    status = api_->image_transfer(&input_texture_->wrapper.image, &input_image_->image.image, 1.0f,
                                  stream_, &transfer_temp_->image.image);
    const int unmap = api_->image_unmap(&input_texture_->wrapper.image, stream_);
    if (status != 0 || unmap != 0) {
        detail = "VSR input transfer failed: " + ErrorText(status != 0 ? status : unmap);
        return false;
    }
    return true;
}

bool NvVsr::ExportOutput(std::string& detail) {
    if (!output_texture_->wrapper_ready) {
        std::memset(&output_texture_->wrapper.image, 0, sizeof(output_texture_->wrapper.image));
        const int init = api_->image_init_d3d(&output_texture_->wrapper.image,
                                              output_texture_->texture.Get());
        if (init != 0) {
            detail = "NvCVImage_InitFromD3D11Texture(output) failed: " + ErrorText(init);
            return false;
        }
        output_texture_->wrapper_ready = true;
    }

    int status = api_->image_map(&output_texture_->wrapper.image, stream_);
    if (status != 0) {
        detail = "VSR output map failed: " + ErrorText(status);
        return false;
    }
    status = api_->image_transfer(&output_image_->image.image, &output_texture_->wrapper.image,
                                  1.0f, stream_, &transfer_temp_->image.image);
    const int unmap = api_->image_unmap(&output_texture_->wrapper.image, stream_);
    const int sync = api_->stream_sync(stream_);
    if (status != 0 || unmap != 0 || sync != 0) {
        detail = "VSR output transfer failed: " +
                 ErrorText(status != 0 ? status : (unmap != 0 ? unmap : sync));
        return false;
    }
    return true;
}

bool NvVsr::Process(const uint8_t* rgba, int width, int height, Frame& out, std::string& detail) {
    if (!effect_ || !rgba || width != in_width_ || height != in_height_) {
        detail = "VSR is not initialized for this frame size";
        return false;
    }

    // NvCVImage_MapResource and the effect run must execute with the CUDA
    // primary context current on this thread.
    const bool pushed = api_->cu_ctx_push(cuda_context_) == 0;
    struct PopScope {
        Api* api;
        bool pushed;
        ~PopScope() {
            if (pushed) {
                void* popped = nullptr;
                api->cu_ctx_pop(&popped);
            }
        }
    } scope{api_.get(), pushed};
    if (!pushed) {
        detail = "cuCtxPushCurrent failed";
        return false;
    }
    if (api_->cuda_set_device) {
        api_->cuda_set_device(cuda_device_);
    }

    const double started = NowMs();
    if (!ImportInput(rgba, width, detail)) {
        return false;
    }
    const int run = api_->run(effect_, 0);
    if (run != 0) {
        detail = "NvVFX_Run failed: " + ErrorText(run);
        return false;
    }
    if (!ExportOutput(detail)) {
        return false;
    }

    out.texture = output_texture_->texture;
    out.srv = output_texture_->srv;
    last_ms_ = NowMs() - started;
    status_ = "upscaling";
    return true;
}

std::string NvVsr::ErrorText(int status) const {
    if (api_ && api_->error_string) {
        if (const char* text = api_->error_string(status)) {
            return text;
        }
    }
    return "status " + std::to_string(status);
}

void NvVsr::Shutdown() {
    if (effect_) {
        DestroyEffect();
    }
    if (api_ && cuda_context_) {
        api_->cu_ctx_push(cuda_context_);
    }
    if (api_) {
        for (auto* image : {&input_image_, &output_image_, &transfer_temp_}) {
            if (*image && (*image)->image.allocated && api_->image_dealloc) {
                api_->image_dealloc(&(*image)->image.image);
                (*image)->image.allocated = false;
            }
        }
        for (auto* slot : {&input_texture_, &output_texture_}) {
            if (*slot && (*slot)->wrapper_ready && api_->image_dealloc) {
                api_->image_dealloc(&(*slot)->wrapper.image);
                (*slot)->wrapper_ready = false;
            }
        }
    }
    input_image_.reset();
    output_image_.reset();
    transfer_temp_.reset();
    input_texture_.reset();
    output_texture_.reset();
    if (stream_ && api_ && api_->stream_destroy) {
        api_->stream_destroy(stream_);
    }
    stream_ = nullptr;
    if (api_ && api_->cu_ctx_pop && cuda_context_) {
        void* popped = nullptr;
        api_->cu_ctx_pop(&popped);
    }
    if (api_ && cuda_context_ && api_->cu_ctx_release) {
        api_->cu_ctx_release(cuda_device_);
    }
    cuda_context_ = nullptr;
    cuda_ready_ = false;
    in_width_ = 0;
    in_height_ = 0;
    out_width_ = 0;
    out_height_ = 0;
    quality_ = -1;
}

}  // namespace brp::media
