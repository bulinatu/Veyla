#include "media/nv_of.h"

#include "media/nvof_shaders.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "nvOpticalFlowD3D12.h"

namespace brp::media {
namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

using CreateInstanceFn = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

}  // namespace

struct NvOf::Api {
    HMODULE module = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST fn{};
};

NvOf::NvOf() = default;

NvOf::~NvOf() {
    Stop();
}

bool NvOf::Start(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height,
                 std::string& detail) {
    Stop();
    if (!device || !queue || width <= 0 || height <= 0) {
        detail = "NvOF needs a device, a queue and a frame size";
        return false;
    }
    device_ = device;
    queue_ = queue;
    width_ = width;
    height_ = height;
    grid_w_ = std::max(1, width / 4);
    grid_h_ = std::max(1, height / 4);

    api_ = std::make_unique<Api>();
    api_->module = ::LoadLibraryExW(L"nvofapi64.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api_->module) {
        detail = "nvofapi64.dll not found";
        return false;
    }
    const auto create = reinterpret_cast<CreateInstanceFn>(
        ::GetProcAddress(api_->module, "NvOFAPICreateInstanceD3D12"));
    using GetMaxFn = NV_OF_STATUS(NVOFAPI*)(uint32_t*);
    const auto get_max = reinterpret_cast<GetMaxFn>(
        ::GetProcAddress(api_->module, "NvOFGetMaxSupportedApiVersion"));
    uint32_t max_version = 0;
    if (get_max) {
        get_max(&max_version);
        std::fprintf(stderr, "[nvof] driver API 0x%X, header 0x%X\n", max_version,
                     NV_OF_API_VERSION);
    }
    if (!create) {
        detail = "NvOFAPICreateInstanceD3D12 missing";
        return false;
    }
    struct FnStorage {
        NV_OF_D3D12_API_FUNCTION_LIST list{};
        unsigned char pad[256]{};
    } storage;
    const bool force40 = std::getenv("VEYLA_NVOF_API40") != nullptr;
    NV_OF_STATUS status = create(force40 ? 0x40 : NV_OF_API_VERSION, &storage.list);
    if (status != NV_OF_SUCCESS && !force40 && max_version &&
        max_version != NV_OF_API_VERSION) {
        status = create(max_version, &storage.list);
    }
    if (status != NV_OF_SUCCESS) {
        // Some drivers expose a newer API but reject the D3D12 instance at
        // that version; 4.0 uses the same struct prefix.
        status = create(0x40, &storage.list);
    }
    api_->fn = storage.list;
    if (status != NV_OF_SUCCESS) {
        detail = "NvOFAPICreateInstanceD3D12 failed (" + std::to_string(status) + ")";
        return false;
    }
    if (!api_->fn.nvCreateOpticalFlowD3D12 ||
        api_->fn.nvCreateOpticalFlowD3D12(device, &of_) != NV_OF_SUCCESS || !of_) {
        detail = "NvCreateOpticalFlowD3D12 failed";
        return false;
    }

    NV_OF_INIT_PARAMS init{};
    init.width = static_cast<uint32_t>(width);
    init.height = static_cast<uint32_t>(height);
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_FAST;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    if (api_->fn.nvOFInit(of_, &init) != NV_OF_SUCCESS) {
        // Last resort: 4.0 instance (the D3D12 structs share the prefix).
        if (api_->fn.nvOFDestroy && of_) {
            api_->fn.nvOFDestroy(of_);
            of_ = nullptr;
        }
        if (create(0x40, &storage.list) == NV_OF_SUCCESS) {
            api_->fn = storage.list;
            if (api_->fn.nvCreateOpticalFlowD3D12 &&
                api_->fn.nvCreateOpticalFlowD3D12(device, &of_) == NV_OF_SUCCESS &&
                of_ && api_->fn.nvOFInit(of_, &init) == NV_OF_SUCCESS) {
                detail = "";
            } else {
                detail = "NvOFInit failed (4.0 fallback)";
                return false;
            }
        } else {
            detail = "NvOFInit failed";
            return false;
        }
    }
    if (api_->fn.nvOFGetSurfaceFormatCountD3D12) {
        uint32_t count = 0;
        if (api_->fn.nvOFGetSurfaceFormatCountD3D12(
                of_, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW, &count) ==
                NV_OF_SUCCESS &&
            count > 0 && count < 16) {
            DXGI_FORMAT formats[16]{};
            if (api_->fn.nvOFGetSurfaceFormatD3D12(
                    of_, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW, formats) ==
                NV_OF_SUCCESS) {
                std::fprintf(stderr, "[nvof] input formats:");
                for (uint32_t i = 0; i < count; ++i) {
                    std::fprintf(stderr, " %u", static_cast<unsigned>(formats[i]));
                }
                std::fprintf(stderr, "\n");
            }
        }
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(grid_w_) * grid_h_ * 4ull;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&flow_)))) {
        detail = "NvOF flow buffer failed";
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator_))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator_.Get(), nullptr,
                                         IID_PPV_ARGS(&list_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&of_fence_)))) {
        detail = "NvOF command objects failed";
        return false;
    }
    list_->Close();
    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    of_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_ || !of_event_) {
        detail = "NvOF fence events failed";
        return false;
    }
    if (!EnsurePipeline(detail) || !RegisterOutput(detail)) {
        return false;
    }
    ready_ = true;
    status_ = "NvOF ready";
    std::fprintf(stderr, "[nvof] %dx%d, grid %dx%d\n", width, height, grid_w_, grid_h_);
    return true;
}

bool NvOf::EnsurePipeline(std::string& detail) {
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 3;
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(::D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                            blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_)))) {
        detail = "NvOF root signature failed";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_.Get();
    pso.CS = {kNvOfMvCs, kNvOfMvCs_size};
    if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)))) {
        detail = "NvOF compute pipeline failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) {
        detail = "NvOF descriptor heap failed";
        return false;
    }
    heap_stride_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool NvOf::RegisterInput(ID3D12Resource* resource, void** handle, std::string& detail) {
    if (!resource || !handle) {
        detail = "NvOF input is null";
        return false;
    }
    for (const auto& entry : inputs_) {
        if (entry.first == resource) {
            *handle = entry.second;
            return true;
        }
    }
    void* registered = nullptr;
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
    params.resource = resource;
    params.inputFencePoint = {of_fence_.Get(), 0};
    params.outputFencePoint = {of_fence_.Get(), 0};
    params.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle*>(&registered);
    if (api_->fn.nvOFRegisterResourceD3D12(of_, &params) != NV_OF_SUCCESS) {
        detail = "NvOFRegisterResourceD3D12(input) failed";
        return false;
    }
    inputs_.push_back({resource, registered});
    *handle = registered;
    return true;
}

bool NvOf::RegisterOutput(std::string& detail) {
    if (!flow_) {
        detail = "NvOF flow buffer is missing";
        return false;
    }
    if (flow_handle_) {
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 unregister{};
        unregister.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle>(flow_handle_);
        api_->fn.nvOFUnregisterResourceD3D12(&unregister);
        flow_handle_ = nullptr;
    }
    const uint64_t value = ++of_value_;
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
    params.resource = flow_.Get();
    params.inputFencePoint = {of_fence_.Get(), 0};
    params.outputFencePoint = {of_fence_.Get(), value};
    params.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle*>(&flow_handle_);
    if (api_->fn.nvOFRegisterResourceD3D12(of_, &params) != NV_OF_SUCCESS) {
        detail = "NvOFRegisterResourceD3D12(output) failed";
        return false;
    }
    return true;
}

bool NvOf::Execute(ID3D12Resource* current, ID3D12Resource* previous,
                   ID3D12Resource* motion_out, std::string& detail) {
    if (!ready_ || !current || !previous || !motion_out) {
        detail = "NvOF is not ready";
        return false;
    }
    const double started = NowMs();
    void* current_handle = nullptr;
    void* previous_handle = nullptr;
    if (!RegisterInput(current, &current_handle, detail) ||
        !RegisterInput(previous, &previous_handle, detail)) {
        return false;
    }
    if (!RegisterOutput(detail)) {
        return false;
    }

    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame = reinterpret_cast<NvOFGPUBufferHandle>(current_handle);
    in.referenceFrame = reinterpret_cast<NvOFGPUBufferHandle>(previous_handle);
    static bool logged_handles = false;
    if (!logged_handles) {
        logged_handles = true;
        std::fprintf(stderr, "[nvof] handles current=%p reference=%p output=%p\n",
                     current_handle, previous_handle, flow_handle_);
    }
    NV_OF_FENCE_POINT input_point{};
    input_point.fence = of_fence_.Get();
    input_point.value = 0;
    in.numFencePoints = 1;
    in.fencePoint = &input_point;
    in.disableTemporalHints = NV_OF_FALSE;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = reinterpret_cast<NvOFGPUBufferHandle>(flow_handle_);
    NV_OF_FENCE_POINT point{};
    point.fence = of_fence_.Get();
    point.value = of_value_;
    out.fencePoint = &point;
    const NV_OF_STATUS status = api_->fn.nvOFExecuteD3D12(of_, &in, &out);
    if (status != NV_OF_SUCCESS) {
        char message[128] = {};
        uint32_t size = sizeof(message);
        if (api_->fn.nvOFGetLastError) {
            api_->fn.nvOFGetLastError(of_, message, &size);
        }
        detail = "NvOFExecuteD3D12 failed (" + std::to_string(status) + "): " +
                 std::string(message);
        return false;
    }
    if (of_fence_->GetCompletedValue() < of_value_) {
        if (FAILED(of_fence_->SetEventOnCompletion(of_value_, of_event_)) ||
            ::WaitForSingleObject(of_event_, 5000) != WAIT_OBJECT_0) {
            detail = "NvOF fence timed out";
            return false;
        }
    }

    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "NvOF compute reset failed";
        return false;
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = flow_.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = motion_out;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list_->ResourceBarrier(2, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += heap_stride_;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += heap_stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = static_cast<UINT>(grid_w_) * grid_h_;
    srv.Buffer.StructureByteStride = 4;
    device_->CreateShaderResourceView(flow_.Get(), &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(motion_out, nullptr, &uav, cpu_uav);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetComputeRootSignature(root_.Get());
    list_->SetPipelineState(pso_.Get());
    list_->SetComputeRootDescriptorTable(0, gpu);
    list_->SetComputeRootDescriptorTable(1, gpu_uav);
    const UINT constants[4] = {static_cast<UINT>(grid_w_), static_cast<UINT>(grid_h_),
                               static_cast<UINT>(width_), static_cast<UINT>(height_)};
    list_->SetComputeRoot32BitConstants(2, 4, constants, 0);
    list_->Dispatch((static_cast<UINT>(width_) + 7) / 8,
                    (static_cast<UINT>(height_) + 7) / 8, 1);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(2, barriers);
    if (!SubmitCompute(detail) || !Wait(detail)) {
        return false;
    }
    last_ms_ = NowMs() - started;
    status_ = "flowing";
    return true;
}

bool NvOf::SubmitCompute(std::string& detail) {
    if (FAILED(list_->Close())) {
        detail = "NvOF compute close failed";
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    submitted_ = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), submitted_))) {
        detail = "NvOF queue signal failed";
        return false;
    }
    return true;
}

bool NvOf::Wait(std::string& detail) {
    if (!submitted_ || !fence_event_) {
        return true;
    }
    if (fence_->GetCompletedValue() < submitted_) {
        if (FAILED(fence_->SetEventOnCompletion(submitted_, fence_event_)) ||
            ::WaitForSingleObject(fence_event_, 5000) != WAIT_OBJECT_0) {
            detail = "NvOF compute fence timed out";
            return false;
        }
    }
    return true;
}

void NvOf::Stop() {
    if (api_ && api_->fn.nvOFUnregisterResourceD3D12) {
        for (const auto& entry : inputs_) {
            if (entry.second) {
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 unregister{};
                unregister.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle>(entry.second);
                api_->fn.nvOFUnregisterResourceD3D12(&unregister);
            }
        }
    }
    inputs_.clear();
    if (flow_handle_ && api_ && api_->fn.nvOFUnregisterResourceD3D12) {
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 unregister{};
        unregister.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle>(flow_handle_);
        api_->fn.nvOFUnregisterResourceD3D12(&unregister);
        flow_handle_ = nullptr;
    }
    if (of_ && api_ && api_->fn.nvOFDestroy) {
        api_->fn.nvOFDestroy(of_);
        of_ = nullptr;
    }
    if (fence_event_) {
        ::CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    if (of_event_) {
        ::CloseHandle(of_event_);
        of_event_ = nullptr;
    }
    flow_.Reset();
    heap_.Reset();
    pso_.Reset();
    root_.Reset();
    list_.Reset();
    allocator_.Reset();
    of_fence_.Reset();
    fence_.Reset();
    ready_ = false;
    submitted_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

}  // namespace brp::media
