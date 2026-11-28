#include "media/ngx_state.h"

#include <atomic>

namespace brp::media {
namespace {

std::atomic<ID3D12Device*> g_device{nullptr};
std::atomic<bool> g_initialized{false};

}  // namespace

bool NgxInitialized() {
    return g_initialized.load();
}

ID3D12Device* NgxInitializedDevice() {
    return g_device.load();
}

void MarkNgxInitialized(ID3D12Device* device) {
    g_device.store(device);
    g_initialized.store(true);
}

}  // namespace brp::media
