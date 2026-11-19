// Minimal CUDA runtime declarations for the TensorRT headers.
//
// The TensorRT C++ headers include <cuda_runtime_api.h> for the stream and
// event types. This project does not link against the CUDA SDK: the runtime
// DLLs are loaded at run time (see src/media/rife.cpp), so only the types the
// headers name are needed here.
#pragma once

#include <cstddef>

typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;

enum cudaMemcpyKind {
    cudaMemcpyHostToHost = 0,
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2,
    cudaMemcpyDeviceToDevice = 3,
    cudaMemcpyDefault = 4,
};

enum cudaStreamCaptureStatus {
    cudaStreamCaptureStatusNone = 0,
    cudaStreamCaptureStatusActive = 1,
    cudaStreamCaptureStatusInvalidated = 2,
};
