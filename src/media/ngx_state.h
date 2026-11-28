// Process-wide NGX state. NGX can be initialized only once per process; the
// DLSS SR and DLSS-G features must share that initialization and its D3D12
// device. Without this, the second Init returns OutOfDate and native DLSS-G is
// disabled for the whole session.
#pragma once

#include <d3d12.h>

namespace brp::media {

bool NgxInitialized();
ID3D12Device* NgxInitializedDevice();
void MarkNgxInitialized(ID3D12Device* device);

}  // namespace brp::media
