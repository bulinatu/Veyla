#include "media/nv_vfx.h"

#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace brp::media {
namespace {

using NvCVStatus = int;

// Opaque storage for NvCVImage. The SDK owns the layout; we only pass the
// pointer back to its API. 512 bytes is well above the real descriptor size.
constexpr int kImageStorage = 512;

struct Img {
    alignas(64) unsigned char bytes[kImageStorage]{};
};

using CreateEffectFn = NvCVStatus (*)(const char*, void**);
using DestroyEffectFn = NvCVStatus (*)(void*);
using SetU32Fn = NvCVStatus (*)(void*, const char*, unsigned);
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

constexpr int kNvcvU8 = 1;
constexpr int kNvcvInterleaved = 0;
constexpr int kNvcvGpu = 2;

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

}  // namespace

struct NvVfx::Image {
    Img image;
    bool allocated = false;
};

struct NvVfx::LinearImage {
    Image image;
};

struct NvVfx::TextureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Image wrapper;
    bool wrapper_ready = false;
};

struct NvVfx::Api {
    CreateEffectFn create_effect = nullptr;
    DestroyEffectFn destroy_effect = nullptr;
    SetU32Fn set_u32 = nullptr;
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
};

NvVfx::NvVfx() = default;

NvVfx::~NvVfx() {
    Shutdown();
}

bool NvVfx::LoadRuntime(const std::wstring& directory, std::string& detail) {
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
        L"nvVFXVideoFrameGeneration.dll",
    };
    HMODULE* targets[] = {nullptr, &ngx_runtime_, &cv_image_, &video_effects_, &vfg_};
    for (int i = 0; i < 5; ++i) {
        HMODULE module = nullptr;
        if (!load(required[i], module, error)) {
            detail = error;
            status_ = detail;
            return false;
        }
        if (targets[i]) {
            *targets[i] = module;
        }
    }

    // NPP is used by the VFG model. Missing ones are tolerated; the error from
    // the effect load is clearer than a loader failure.
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
        std::fprintf(stderr, "[vfx] runtime version 0x%08X\n", version);
        if (version == 0) {
            detail = "NVIDIA VFX runtime version is unavailable";
            status_ = detail;
            return false;
        }
    }

    runtime_loaded_ = true;
    status_ = "runtime loaded";
    return true;
}

bool NvVfx::LoadModule(const std::wstring& directory, const wchar_t* name, HMODULE& module,
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
        std::snprintf(text, sizeof(text), "cannot load %ls (error %lu)",
                      name, static_cast<unsigned long>(::GetLastError()));
        detail = text;
        return false;
    }
    return true;
}

bool NvVfx::ResolveApi(std::string& detail) {
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

    if (!ok) {
        detail = "NVIDIA VFX runtime is missing required exports";
        return false;
    }
    return true;
}

bool NvVfx::CreateCudaContext(ID3D11Device* device, std::string& detail) {
    if (cuda_ready_) {
        return true;
    }
    if (!api_ || !api_->cu_init || !device) {
        detail = "NVIDIA VFX API not loaded";
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
        // Fall back to device 0 only when the renderer is the primary NVIDIA
        // adapter; otherwise interop would fail later with a worse error.
        if (api_->cu_device_get(&cuda_device, 0) != 0) {
            detail = "CUDA device selection failed";
            return false;
        }
        std::fprintf(stderr, "[vfx] cuD3D11GetDevice failed; using CUDA device 0\n");
    }

    int major = 0;
    int minor = 0;
    api_->cu_attribute(&major, 75, cuda_device);
    api_->cu_attribute(&minor, 76, cuda_device);
    const bool supported = (major == 8 && minor == 9) || major == 10 || major == 12;
    if (!supported) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "NVIDIA VFG needs an Ada or Blackwell GPU (found SM %d.%d)", major, minor);
        detail = text;
        return false;
    }

    if (api_->cu_ctx_retain(&cuda_context_, cuda_device) != 0 || !cuda_context_) {
        detail = "cuDevicePrimaryCtxRetain failed";
        return false;
    }
    cuda_device_ = cuda_device;
    cuda_ready_ = true;
    std::fprintf(stderr, "[vfx] CUDA device %d, SM %d.%d\n", cuda_device, major, minor);
    return true;
}

bool NvVfx::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
                       int model, std::string& detail) {
    if (!runtime_loaded_ || !device || !context || width <= 0 || height <= 0) {
        detail = "NVIDIA VFX is not initialized";
        return false;
    }
    if (effect_ && width == width_ && height == height_ && model == model_) {
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

    linear_.clear();
    for (int i = 0; i < 3; ++i) {
        auto image = std::make_unique<LinearImage>();
        int alloc = api_->image_alloc(&image->image.image, static_cast<unsigned>(width),
                                      static_cast<unsigned>(height), pixel_format_, kNvcvU8,
                                      kNvcvInterleaved, kNvcvGpu, 32);
        if (alloc != 0 && i == 0) {
            // NVIDIA's enum values changed in older builds; try BGRA and then
            // the 4-channel formats before giving up.
            pixel_format_ = 7;
            alloc = api_->image_alloc(&image->image.image, static_cast<unsigned>(width),
                                      static_cast<unsigned>(height), pixel_format_, kNvcvU8,
                                      kNvcvInterleaved, kNvcvGpu, 32);
        }
        if (alloc != 0) {
            detail = "NvCVImage_Alloc failed: " + ErrorText(alloc);
            return false;
        }
        image->image.allocated = true;
        linear_.push_back(std::move(image));
    }
    transfer_temp_ = std::make_unique<LinearImage>();

    if (!CreateEffect(width, height, model, detail)) {
        return false;
    }
    if (!CreateInputTextures(width, height, detail) ||
        !CreateOutputTextures(width, height, detail)) {
        return false;
    }

    width_ = width;
    height_ = height;
    model_ = model;
    previous_index_ = 0;
    current_index_ = 1;
    has_previous_ = false;
    in_index_ = 0;
    out_index_ = 0;
    failures_ = 0;
    std::fprintf(stderr,
                 "[vfx] initialized %dx%d, model %d, format %d, multiplier per call\n",
                 width_, height_, model_, pixel_format_);
    return true;
}

bool NvVfx::CreateEffect(int width, int height, int model, std::string& detail) {
    int status = api_->create_effect("VideoFrameGeneration", &effect_);
    if (status != 0 || !effect_) {
        status = api_->create_effect("Video Frame Generation", &effect_);
    }
    if (status != 0 || !effect_) {
        detail = "NvVFX_CreateEffect(VideoFrameGeneration) failed: " + ErrorText(status);
        return false;
    }

    status = api_->set_cuda_stream(effect_, "CudaStream", stream_);
    if (status != 0) {
        detail = "VFG CudaStream failed: " + ErrorText(status);
        return false;
    }

    auto set_u32 = [&](const char* name, unsigned value) {
        const int result = api_->set_u32(effect_, name, value);
        if (result != 0) {
            std::fprintf(stderr, "[vfx] %s=%u failed: %s\n", name, value,
                         ErrorText(result).c_str());
        }
        return result == 0;
    };
    if (!set_u32("InputWidth", static_cast<unsigned>(width)) ||
        !set_u32("InputHeight", static_cast<unsigned>(height)) ||
        !set_u32("Mode", static_cast<unsigned>(model)) ||
        !set_u32("BatchSize", 1) || !set_u32("FrameMultiplier", 2) ||
        !set_u32("AutomaticShotChangeDetectionEnabled", 1)) {
        detail = "VFG parameter setup failed";
        return false;
    }

    // Bind the three images. NVIDIA documents both descriptive and short keys;
    // probe the short ones first, then fall back.
    set_u32("FrameMultiplier", 2);
    const int bind1 = api_->set_image(effect_, "SrcImage0", &linear_[0]->image.image);
    const int bind2 = api_->set_image(effect_, "SrcImage1", &linear_[1]->image.image);
    const int bind3 = api_->set_image(effect_, "DstImage0", &linear_[2]->image.image);
    if (bind1 != 0 || bind2 != 0 || bind3 != 0) {
        const int a = api_->set_image(effect_, "PreviousFrame", &linear_[0]->image.image);
        const int b = api_->set_image(effect_, "CurrentFrame", &linear_[1]->image.image);
        const int c = api_->set_image(effect_, "OutputFrame", &linear_[2]->image.image);
        if (a != 0 || b != 0 || c != 0) {
            detail = "VFG image binding failed";
            return false;
        }
    }

    status = api_->load(effect_);
    if (status != 0) {
        detail = "NvVFX_Load failed: " + ErrorText(status);
        return false;
    }
    return true;
}

void NvVfx::DestroyEffect() {
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

bool NvVfx::CreateInputTextures(int width, int height, std::string& detail) {
    inputs_.clear();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Four textures: the previous/current ping-pong plus headroom for frames
    // still queued for presentation.
    for (int i = 0; i < 4; ++i) {
        auto slot = std::make_unique<TextureSlot>();
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &slot->texture))) {
            detail = "VFG input texture creation failed";
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(slot->texture.Get(), nullptr, &slot->srv))) {
            detail = "VFG input SRV creation failed";
            return false;
        }
        inputs_.push_back(std::move(slot));
    }
    return true;
}

bool NvVfx::CreateOutputTextures(int width, int height, std::string& detail) {
    outputs_.clear();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    // Up to three generated frames per pair; six slots give a full ring even
    // when presentation falls behind.
    for (int i = 0; i < 6; ++i) {
        auto slot = std::make_unique<TextureSlot>();
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &slot->texture))) {
            detail = "VFG output texture creation failed";
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(slot->texture.Get(), nullptr, &slot->srv))) {
            detail = "VFG output SRV creation failed";
            return false;
        }
        outputs_.push_back(std::move(slot));
    }
    return true;
}

bool NvVfx::ImportTexture(ID3D11Texture2D* texture, Image& destination, std::string& detail) {
    if (!texture || !destination.allocated) {
        detail = "VFG import: invalid texture/image";
        return false;
    }
    TextureSlot* slot = nullptr;
    for (auto& candidate : inputs_) {
        if (candidate->texture.Get() == texture) {
            slot = candidate.get();
            break;
        }
    }
    if (!slot) {
        detail = "VFG import: texture is not owned by the generator";
        return false;
    }
    if (!slot->wrapper_ready) {
        std::memset(&slot->wrapper.image, 0, sizeof(slot->wrapper.image));
        const int init = api_->image_init_d3d(&slot->wrapper.image, texture);
        if (init != 0) {
            detail = "NvCVImage_InitFromD3D11Texture failed: " + ErrorText(init);
            return false;
        }
        slot->wrapper_ready = true;
    }

    context_->Flush();
    int status = api_->image_map(&slot->wrapper.image, stream_);
    if (status != 0) {
        detail = "NvCVImage_MapResource failed: " + ErrorText(status);
        return false;
    }
    status = api_->image_transfer(&slot->wrapper.image, &destination.image, 1.0f, stream_,
                                  &transfer_temp_->image.image);
    const int unmap = api_->image_unmap(&slot->wrapper.image, stream_);
    if (status != 0 || unmap != 0) {
        detail = "VFG import transfer failed: " + ErrorText(status != 0 ? status : unmap);
        return false;
    }
    return true;
}

bool NvVfx::ExportTexture(Image& source, ID3D11Texture2D* texture, std::string& detail) {
    TextureSlot* slot = nullptr;
    for (auto& candidate : outputs_) {
        if (candidate->texture.Get() == texture) {
            slot = candidate.get();
            break;
        }
    }
    if (!slot || !source.allocated) {
        detail = "VFG export: invalid texture/image";
        return false;
    }
    if (!slot->wrapper_ready) {
        std::memset(&slot->wrapper.image, 0, sizeof(slot->wrapper.image));
        const int init = api_->image_init_d3d(&slot->wrapper.image, texture);
        if (init != 0) {
            detail = "NvCVImage_InitFromD3D11Texture(output) failed: " + ErrorText(init);
            return false;
        }
        slot->wrapper_ready = true;
    }

    int status = api_->image_map(&slot->wrapper.image, stream_);
    if (status != 0) {
        detail = "NvCVImage_MapResource(output) failed: " + ErrorText(status);
        return false;
    }
    status = api_->image_transfer(&source.image, &slot->wrapper.image, 1.0f, stream_,
                                  &transfer_temp_->image.image);
    const int unmap = api_->image_unmap(&slot->wrapper.image, stream_);
    const int sync = api_->stream_sync(stream_);
    if (status != 0 || unmap != 0 || sync != 0) {
        detail = "VFG export transfer failed: " +
                 ErrorText(status != 0 ? status : (unmap != 0 ? unmap : sync));
        return false;
    }
    return true;
}

bool NvVfx::SetImages(Image& previous, Image& current, Image& output, std::string& detail) {
    int status = api_->set_image(effect_, "SrcImage0", &previous.image);
    if (status == 0) {
        status = api_->set_image(effect_, "SrcImage1", &current.image);
    }
    if (status == 0) {
        status = api_->set_image(effect_, "DstImage0", &output.image);
    }
    if (status != 0) {
        int alias = api_->set_image(effect_, "PreviousFrame", &previous.image);
        if (alias == 0) alias = api_->set_image(effect_, "CurrentFrame", &current.image);
        if (alias == 0) alias = api_->set_image(effect_, "OutputFrame", &output.image);
        if (alias != 0) {
            detail = "VFG SetImage failed: " + ErrorText(status != 0 ? status : alias);
            return false;
        }
    }
    return true;
}

bool NvVfx::PushFrame(const uint8_t* rgba, int width, int height, int multiplier,
                      std::vector<Frame>& sequence, std::string& detail) {
    sequence.clear();
    if (!effect_ || !rgba || width != width_ || height != height_) {
        detail = "VFG is not initialized for this frame size";
        return false;
    }
    if (multiplier < 2) {
        detail = "VFG multiplier must be at least 2";
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

    const double started = NowMs();
    TextureSlot& input = *inputs_[in_index_];
    in_index_ = (in_index_ + 1) % static_cast<int>(inputs_.size());
    context_->UpdateSubresource(input.texture.Get(), 0, nullptr, rgba,
                                static_cast<UINT>(width) * 4u, 0);

    Image& current = linear_[current_index_]->image;
    if (!ImportTexture(input.texture.Get(), current, detail)) {
        return false;
    }

    if (!has_previous_) {
        sequence.push_back({input.texture, input.srv, false});
        has_previous_ = true;
        std::swap(previous_index_, current_index_);
        last_generate_ms_ = NowMs() - started;
        return true;
    }

    Image& previous = linear_[previous_index_]->image;
    Image& output = linear_[2]->image;
    if (!SetImages(previous, current, output, detail)) {
        return false;
    }

    api_->set_u32(effect_, "FrameMultiplier", static_cast<unsigned>(multiplier));
    api_->set_u32(effect_, "ShotChange", 0);

    for (int i = 1; i < multiplier; ++i) {
        if (api_->set_u32(effect_, "FrameIndex", static_cast<unsigned>(i)) != 0) {
            detail = "VFG FrameIndex failed";
            return false;
        }
        const int run = api_->run(effect_, 0);
        if (run != 0) {
            detail = "NvVFX_Run failed: " + ErrorText(run);
            return false;
        }
        TextureSlot& out = *outputs_[out_index_];
        out_index_ = (out_index_ + 1) % static_cast<int>(outputs_.size());
        if (!ExportTexture(output, out.texture.Get(), detail)) {
            ++failures_;
            return false;
        }
        sequence.push_back({out.texture, out.srv, true});
    }

    sequence.push_back({input.texture, input.srv, false});
    std::swap(previous_index_, current_index_);
    last_generate_ms_ = NowMs() - started;
    status_ = "generating";
    return true;
}

std::string NvVfx::ErrorText(int status) const {
    if (api_ && api_->error_string) {
        if (const char* text = api_->error_string(status)) {
            return text;
        }
    }
    return "status " + std::to_string(status);
}

void NvVfx::Shutdown() {
    if (effect_) {
        DestroyEffect();
    }
    if (api_ && cuda_context_) {
        api_->cu_ctx_push(cuda_context_);
    }
    if (api_) {
        for (auto& image : linear_) {
            if (image && image->image.allocated && api_->image_dealloc) {
                api_->image_dealloc(&image->image.image);
                image->image.allocated = false;
            }
        }
        for (auto& slot : inputs_) {
            if (slot && slot->wrapper_ready && api_->image_dealloc) {
                api_->image_dealloc(&slot->wrapper.image);
                slot->wrapper_ready = false;
            }
        }
        for (auto& slot : outputs_) {
            if (slot && slot->wrapper_ready && api_->image_dealloc) {
                api_->image_dealloc(&slot->wrapper.image);
                slot->wrapper_ready = false;
            }
        }
        if (transfer_temp_ && api_->image_dealloc) {
            api_->image_dealloc(&transfer_temp_->image.image);
        }
    }
    linear_.clear();
    inputs_.clear();
    outputs_.clear();
    transfer_temp_.reset();
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
    has_previous_ = false;
    width_ = 0;
    height_ = 0;
    failures_ = 0;
}

}  // namespace brp::media
