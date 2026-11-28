#pragma once

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nvOpticalFlowCommon.h"

namespace brp::media {

// NVIDIA Optical Flow Accelerator (NVOFA) on D3D12.
//
// Produces a dense R16G16_FLOAT motion-vector texture for DLSS-G / DLSS SR
// from the two most recent video frames. The flow grid is 4x4 and is expanded
// to full resolution by a compute shader.
class NvOf {
public:
    NvOf();
    ~NvOf();

    NvOf(const NvOf&) = delete;
    NvOf& operator=(const NvOf&) = delete;

    bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height,
               std::string& detail);
    void Stop();

    // current/previous and motion_out live on the same device. The caller must
    // have completed the upload of current and previous before calling.
    bool Execute(ID3D12Resource* current, ID3D12Resource* previous,
                 ID3D12Resource* motion_out, std::string& detail);

    bool ready() const { return of_ != nullptr; }
    int grid_width() const { return grid_w_; }
    int grid_height() const { return grid_h_; }
    const std::string& status() const { return status_; }
    double last_ms() const { return last_ms_; }

private:
    struct Api;
    bool EnsurePipeline(std::string& detail);
    bool RegisterInput(ID3D12Resource* resource, void** handle, std::string& detail);
    bool RegisterOutput(std::string& detail);
    bool SubmitCompute(std::string& detail);
    bool Wait(std::string& detail);

    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;
    int grid_w_ = 0;
    int grid_h_ = 0;
    double last_ms_ = 0.0;
    std::string status_;

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;
    UINT64 submitted_ = 0;
    HANDLE fence_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Fence> of_fence_;
    UINT64 of_value_ = 0;
    HANDLE of_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> flow_;
    void* flow_handle_ = nullptr;
    std::vector<std::pair<ID3D12Resource*, void*>> inputs_;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT heap_stride_ = 0;

    NvOFHandle of_ = nullptr;
    std::unique_ptr<Api> api_;
};

}  // namespace brp::media
