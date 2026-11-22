#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace brp::next {

struct GpuInfo {
    bool present = false;
    bool nvidia = false;
    std::string name;
    std::string driver;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t vram_mb = 0;
};

struct DisplayInfo {
    std::wstring name;
    std::wstring device;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
    bool primary = false;
    RECT bounds{};
};

std::vector<std::string> EnumerateAdapterNames();

struct SystemInfo {
    std::string os;
    std::string cpu;
    int logical_cores = 0;
    std::uint64_t ram_mb = 0;
    GpuInfo gpu;
    std::vector<DisplayInfo> displays;
    bool dlss_sr = false;
    bool rife_lite = false;
    bool rife_heavy = false;
    bool tensorrt_runtime = false;
    bool cuda = false;
    bool nr_worker = false;
    bool dlss_fg = false;
    bool dlss_fg_multi = false;
    std::wstring nr_dir;
    std::wstring exe_dir;
};

SystemInfo ProbeSystem();

}  // namespace brp::next
