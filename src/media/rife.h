#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brp::media {

// RIFE frame interpolation through TensorRT.
//
// Both the TensorRT runtime and the CUDA runtime are loaded from a directory
// the user provides (BRP_TENSORRT_DIR or an explicit path). Nothing is linked:
// the factory functions are resolved with GetProcAddress and everything else
// goes through the interfaces in the TensorRT headers. The engine is built
// once with trtexec for the target resolution and GPU, then deserialized here.
class Rife {
public:
    Rife();
    ~Rife();
    Rife(const Rife&) = delete;
    Rife& operator=(const Rife&) = delete;

    // runtime_dir: folder with nvinfer_10.dll and cudart64_13.dll.
    // engine_path: a .engine built for the same GPU and shape.
    bool Load(const std::string& runtime_dir, const std::string& engine_path,
              int width, int height, std::string& detail);

    // width/height are the source frame size. The engine may be larger (RIFE
    // wants multiples of 64 in height): rows are padded and cropped back.
    bool Process(const uint8_t* rgba_a, const uint8_t* rgba_b, float timestep,
                 std::vector<uint8_t>& out, std::string& detail);

    bool ready() const { return ready_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int engine_width() const { return engine_width_; }
    int engine_height() const { return engine_height_; }
    double last_ms() const { return last_ms_; }
    double last_convert_ms() const { return last_convert_ms_; }

private:
    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;
    int engine_width_ = 0;
    int engine_height_ = 0;
    double last_ms_ = 0.0;
    double last_convert_ms_ = 0.0;

    void* nvinfer_ = nullptr;   // HMODULE
    void* cudart_ = nullptr;    // HMODULE
    void* runtime_ = nullptr;   // nvinfer1::IRuntime
    void* engine_ = nullptr;    // nvinfer1::ICudaEngine
    void* context_ = nullptr;   // nvinfer1::IExecutionContext
    void* stream_ = nullptr;    // cudaStream_t
    void* d_in0_ = nullptr;
    void* d_in1_ = nullptr;
    void* d_timestep_ = nullptr;
    void* d_out_ = nullptr;
    size_t plane_bytes_ = 0;
    bool combined_ = false;      // one 6-channel input (imgA + imgB) instead of img0/img1
    int input_planes_ = 3;
    std::string input_name_;
    std::string output_name_;

    // Page-locked host buffers: with pageable memory the driver copies
    // through a staging buffer, which cost about 7 ms per frame at 1080p.
    float* host_in0_ = nullptr;
    float* host_in1_ = nullptr;
    float* host_out_ = nullptr;
};

}  // namespace brp::media
