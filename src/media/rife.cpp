#include "media/rife.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "NvInfer.h"

namespace brp::media {
namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class RifeLogger : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR) {
            std::fprintf(stderr, "[rife] %s\n", msg);
        }
    }
};

struct CudaApi {
    using Malloc = int (*)(void**, size_t);
    using Free = int (*)(void*);
    using MemcpyAsync = int (*)(void*, const void*, size_t, int, cudaStream_t);
    using StreamCreate = int (*)(cudaStream_t*);
    using StreamSync = int (*)(cudaStream_t);
    using StreamDestroy = int (*)(cudaStream_t);

    Malloc malloc = nullptr;
    Free free = nullptr;
    MemcpyAsync memcpy_async = nullptr;
    StreamCreate stream_create = nullptr;
    StreamSync stream_sync = nullptr;
    StreamDestroy stream_destroy = nullptr;

    static constexpr int kHostToDevice = 1;
    static constexpr int kDeviceToHost = 2;

    bool Load(void* module, std::string& detail) {
        HMODULE m = static_cast<HMODULE>(module);
        malloc = reinterpret_cast<Malloc>(::GetProcAddress(m, "cudaMalloc"));
        free = reinterpret_cast<Free>(::GetProcAddress(m, "cudaFree"));
        memcpy_async = reinterpret_cast<MemcpyAsync>(::GetProcAddress(m, "cudaMemcpyAsync"));
        stream_create = reinterpret_cast<StreamCreate>(::GetProcAddress(m, "cudaStreamCreate"));
        stream_sync =
            reinterpret_cast<StreamSync>(::GetProcAddress(m, "cudaStreamSynchronize"));
        stream_destroy = reinterpret_cast<StreamDestroy>(::GetProcAddress(m, "cudaStreamDestroy"));
        if (!malloc || !free || !memcpy_async || !stream_create || !stream_sync ||
            !stream_destroy) {
            detail = "cudart is missing one of the symbols RIFE needs";
            return false;
        }
        return true;
    }
};

RifeLogger g_logger;
CudaApi g_cuda;

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    }
    return out;
}

}  // namespace

Rife::Rife() = default;

Rife::~Rife() {
    if (context_) {
        delete static_cast<nvinfer1::IExecutionContext*>(context_);
        context_ = nullptr;
    }
    if (engine_) {
        delete static_cast<nvinfer1::ICudaEngine*>(engine_);
        engine_ = nullptr;
    }
    if (runtime_) {
        delete static_cast<nvinfer1::IRuntime*>(runtime_);
        runtime_ = nullptr;
    }
    if (g_cuda.free) {
        for (void** p : {&d_in0_, &d_in1_, &d_timestep_, &d_out_}) {
            if (*p) {
                g_cuda.free(*p);
                *p = nullptr;
            }
        }
    }
    if (stream_ && g_cuda.stream_destroy) {
        g_cuda.stream_destroy(static_cast<cudaStream_t>(stream_));
        stream_ = nullptr;
    }
    if (cudart_) {
        ::FreeLibrary(static_cast<HMODULE>(cudart_));
        cudart_ = nullptr;
    }
    if (nvinfer_) {
        ::FreeLibrary(static_cast<HMODULE>(nvinfer_));
        nvinfer_ = nullptr;
    }
}

bool Rife::Load(const std::string& runtime_dir, const std::string& engine_path,
                std::string& detail) {
    ready_ = false;
    if (runtime_dir.empty()) {
        detail = "no TensorRT runtime directory (set BRP_TENSORRT_DIR or pass --runtime)";
        return false;
    }

    const std::string dir = runtime_dir;
    const std::string infer_path = dir + "\\nvinfer_10.dll";
    const std::string cudart_path = dir + "\\cudart64_13.dll";
    nvinfer_ = ::LoadLibraryW(Widen(infer_path).c_str());
    if (!nvinfer_) {
        detail = "cannot load " + infer_path;
        return false;
    }
    cudart_ = ::LoadLibraryW(Widen(cudart_path).c_str());
    if (!cudart_) {
        detail = "cannot load " + cudart_path;
        return false;
    }
    if (!g_cuda.Load(cudart_, detail)) {
        return false;
    }

    using CreateRuntime = nvinfer1::IRuntime* (*)(nvinfer1::ILogger&, int32_t);
    auto create_runtime = reinterpret_cast<CreateRuntime>(
        ::GetProcAddress(static_cast<HMODULE>(nvinfer_), "createInferRuntime_INTERNAL"));
    if (!create_runtime) {
        detail = "nvinfer_10.dll has no createInferRuntime_INTERNAL";
        return false;
    }
    constexpr int32_t kTensorRtVersion =
        NV_TENSORRT_MAJOR * 10000 + NV_TENSORRT_MINOR * 100 + NV_TENSORRT_PATCH;
    auto* runtime = create_runtime(g_logger, kTensorRtVersion);
    if (!runtime) {
        detail = "createInferRuntime failed";
        return false;
    }
    runtime_ = runtime;

    std::ifstream file(engine_path, std::ios::binary | std::ios::ate);
    if (!file) {
        detail = "cannot open " + engine_path;
        return false;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> blob(static_cast<size_t>(size));
    if (!file.read(blob.data(), size)) {
        detail = "cannot read " + engine_path;
        return false;
    }
    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(blob.data(), blob.size());
    if (!engine) {
        detail = "deserializeCudaEngine failed (engine built for another TensorRT or GPU?)";
        return false;
    }
    engine_ = engine;
    nvinfer1::IExecutionContext* context = engine->createExecutionContext();
    if (!context) {
        detail = "createExecutionContext failed";
        return false;
    }
    context_ = context;

    // Read the shapes instead of trusting the caller: the engine is the truth.
    for (int32_t i = 0; i < engine->getNbIOTensors(); ++i) {
        const char* name = engine->getIOTensorName(i);
        const nvinfer1::Dims dims = engine->getTensorShape(name);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT &&
            std::strcmp(name, "img0") == 0) {
            if (dims.nbDims != 4) {
                detail = "img0 is not NCHW";
                return false;
            }
            engine_height_ = dims.d[2];
            engine_width_ = dims.d[3];
        }
    }
    if (engine_width_ <= 0 || engine_height_ <= 0) {
        detail = "engine has no img0 input";
        return false;
    }
    width_ = engine_width_;
    height_ = engine_height_;

    plane_bytes_ = static_cast<size_t>(engine_width_) * engine_height_ * sizeof(float);
    const size_t plane_count = 3;
    if (g_cuda.malloc(&d_in0_, plane_bytes_ * plane_count) != 0 ||
        g_cuda.malloc(&d_in1_, plane_bytes_ * plane_count) != 0 ||
        g_cuda.malloc(&d_timestep_, sizeof(float)) != 0 ||
        g_cuda.malloc(&d_out_, plane_bytes_ * plane_count) != 0) {
        detail = "cudaMalloc failed";
        return false;
    }
    cudaStream_t stream = nullptr;
    if (g_cuda.stream_create(&stream) != 0) {
        detail = "cudaStreamCreate failed";
        return false;
    }
    stream_ = stream;

    host_in0_.resize(static_cast<size_t>(engine_width_) * engine_height_ * plane_count);
    host_in1_.resize(host_in0_.size());
    host_out_.resize(host_in0_.size());

    context->setTensorAddress("img0", d_in0_);
    context->setTensorAddress("img1", d_in1_);
    context->setTensorAddress("timestep", d_timestep_);
    context->setTensorAddress("frame", d_out_);

    ready_ = true;
    detail = "engine " + std::to_string(engine_width_) + "x" +
             std::to_string(engine_height_);
    return true;
}

bool Rife::Process(const uint8_t* rgba_a, const uint8_t* rgba_b, float timestep,
                   std::vector<uint8_t>& out, std::string& detail) {
    if (!ready_) {
        detail = "RIFE engine not loaded";
        return false;
    }
    if (!rgba_a || !rgba_b) {
        detail = "no input frames";
        return false;
    }
    const auto started = Clock::now();
    const auto convert_start = started;

    // RGBA8 -> float CHW in 0..1. Rows past the source height repeat the last
    // row, so an engine padded to a multiple of 64 sees no black band.
    const int w = engine_width_;
    const int h = engine_height_;
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(y, height_ - 1);
        const uint8_t* row_a = rgba_a + static_cast<size_t>(sy) * width_ * 4;
        const uint8_t* row_b = rgba_b + static_cast<size_t>(sy) * width_ * 4;
        const int row_w = std::min(w, width_);
        for (int x = 0; x < row_w; ++x) {
            const size_t src = static_cast<size_t>(x) * 4;
            const size_t dst = static_cast<size_t>(y) * w + x;
            host_in0_[dst] = row_a[src] * (1.0f / 255.0f);
            host_in0_[dst + static_cast<size_t>(w) * h] =
                row_a[src + 1] * (1.0f / 255.0f);
            host_in0_[dst + static_cast<size_t>(w) * h * 2] =
                row_a[src + 2] * (1.0f / 255.0f);
            host_in1_[dst] = row_b[src] * (1.0f / 255.0f);
            host_in1_[dst + static_cast<size_t>(w) * h] =
                row_b[src + 1] * (1.0f / 255.0f);
            host_in1_[dst + static_cast<size_t>(w) * h * 2] =
                row_b[src + 2] * (1.0f / 255.0f);
        }
    }
    last_convert_ms_ = MsSince(convert_start);

    auto* context = static_cast<nvinfer1::IExecutionContext*>(context_);
    cudaStream_t stream = static_cast<cudaStream_t>(stream_);
    const size_t bytes = plane_bytes_ * 3;
    if (g_cuda.memcpy_async(d_in0_, host_in0_.data(), bytes, CudaApi::kHostToDevice, stream) != 0 ||
        g_cuda.memcpy_async(d_in1_, host_in1_.data(), bytes, CudaApi::kHostToDevice, stream) != 0 ||
        g_cuda.memcpy_async(d_timestep_, &timestep, sizeof(float), CudaApi::kHostToDevice,
                            stream) != 0) {
        detail = "upload failed";
        return false;
    }
    if (!context->enqueueV3(stream)) {
        detail = "enqueueV3 failed";
        return false;
    }
    if (g_cuda.memcpy_async(host_out_.data(), d_out_, bytes, CudaApi::kDeviceToHost, stream) != 0) {
        detail = "download failed";
        return false;
    }
    if (g_cuda.stream_sync(stream) != 0) {
        detail = "cudaStreamSynchronize failed";
        return false;
    }

    out.resize(static_cast<size_t>(width_) * height_ * 4);
    const size_t plane = static_cast<size_t>(w) * h;
    for (int y = 0; y < height_; ++y) {
        uint8_t* dst = out.data() + static_cast<size_t>(y) * width_ * 4;
        for (int x = 0; x < width_; ++x) {
            const size_t src = static_cast<size_t>(y) * w + x;
            for (int c = 0; c < 3; ++c) {
                const float v = host_out_[src + plane * static_cast<size_t>(c)];
                dst[static_cast<size_t>(x) * 4 + c] =
                    static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            dst[static_cast<size_t>(x) * 4 + 3] = 255;
        }
    }
    last_ms_ = MsSince(started);
    return true;
}

}  // namespace brp::media
