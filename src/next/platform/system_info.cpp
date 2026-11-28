#include "platform/system_info.h"

#include <windows.h>

#include <dxgi1_4.h>
#include <winternl.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

namespace brp::next {
namespace {

using Microsoft::WRL::ComPtr;

std::string Narrow(const wchar_t* text) {
    if (!text) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string RegistryString(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer);
    if (::RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buffer, &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    return Narrow(buffer);
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}

struct MonitorCollect {
    std::vector<DisplayInfo>* out = nullptr;
};

BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* collect = reinterpret_cast<MonitorCollect*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    DisplayInfo display;
    display.name = info.szDevice;
    display.device = info.szDevice;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    display.bounds = info.rcMonitor;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        display.width = static_cast<int>(mode.dmPelsWidth);
        display.height = static_cast<int>(mode.dmPelsHeight);
        display.refresh_hz = static_cast<int>(mode.dmDisplayFrequency);
    }
    collect->out->push_back(display);
    return TRUE;
}

std::string ProbeDriverVersion() {
    const wchar_t* class_path =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
        L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_path, 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return {};
    }
    std::string version;
    for (DWORD index = 0; index < 64; ++index) {
        wchar_t subkey[64] = {};
        DWORD length = static_cast<DWORD>(std::size(subkey));
        if (::RegEnumKeyExW(root, index, subkey, &length, nullptr, nullptr, nullptr,
                            nullptr) != ERROR_SUCCESS) {
            break;
        }
        const std::wstring path = std::wstring(class_path) + L"\\" + subkey;
        const std::string provider =
            RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"ProviderName");
        if (provider.find("NVIDIA") == std::string::npos) {
            continue;
        }
        version = RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"DriverVersion");
        if (!version.empty()) {
            break;
        }
    }
    ::RegCloseKey(root);
    return version;
}

GpuInfo ProbeGpu() {
    GpuInfo info;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return info;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        info.present = true;
        info.name = Narrow(desc.Description);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.vram_mb = static_cast<std::uint64_t>(desc.DedicatedVideoMemory) / (1024 * 1024);
        info.nvidia = desc.VendorId == 0x10DE;
        info.amd = desc.VendorId == 0x1002;
        info.intel = desc.VendorId == 0x8086;
        info.driver = ProbeDriverVersion();
        break;
    }
    return info;
}

std::string ProbeOs() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return "Windows";
    }
    const auto fn = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) {
        return "Windows";
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (fn(&version) != 0) {
        return "Windows";
    }
    const std::string family =
        version.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    return family + " build " + std::to_string(version.dwBuildNumber);
}

}  // namespace

SystemInfo ProbeSystem() {
    SystemInfo info;
    info.os = ProbeOs();
    info.cpu = RegistryString(HKEY_LOCAL_MACHINE,
                              L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                              L"ProcessorNameString");
    SYSTEM_INFO system{};
    ::GetNativeSystemInfo(&system);
    info.logical_cores = static_cast<int>(system.dwNumberOfProcessors);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        info.ram_mb = memory.ullTotalPhys / (1024 * 1024);
    }
    info.gpu = ProbeGpu();

    MonitorCollect collect{&info.displays};
    ::EnumDisplayMonitors(nullptr, nullptr, MonitorProc,
                          reinterpret_cast<LPARAM>(&collect));

    info.exe_dir = ExeDirectory();
    const std::filesystem::path dir(info.exe_dir);
    std::error_code ec;
    info.dlss_sr = std::filesystem::exists(dir / "nvngx_dlss.dll", ec);
    info.nr_worker = false;
    std::vector<std::filesystem::path> nr_candidates;
    nr_candidates.push_back(dir / "nr" / "native");
    nr_candidates.push_back(dir / ".." / ".." / "third_party" / "neuralscreen" /
                            "native");
    std::error_code cwd_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    if (!cwd_ec) {
        nr_candidates.push_back(cwd / "third_party" / "neuralscreen" / "native");
    }
    for (const auto& candidate : nr_candidates) {
        if (std::filesystem::exists(candidate / "nvngx.dll", ec)) {
            std::error_code abs_ec;
            const std::filesystem::path absolute =
                std::filesystem::absolute(candidate, abs_ec);
            info.nr_dir = (abs_ec ? candidate : absolute).wstring();
            info.nr_worker = true;
            info.dlss_fg = std::filesystem::exists(candidate / "nvngx_dlssg.dll", ec);
            info.dlss_fg_multi =
                std::filesystem::exists(candidate / "fg" / "winmm.dll", ec);
            break;
        }
    }

    // AMD FSR SDK 2.4: loader + upscaler + frame generation signed DLLs.
    std::vector<std::filesystem::path> fsr_candidates;
    fsr_candidates.push_back(dir / "amd-fsr");
    fsr_candidates.push_back(dir / ".." / "third_party" / "amd-fsr" / "signedbin");
    fsr_candidates.push_back(dir / "third_party" / "amd-fsr" / "signedbin");
    if (!cwd_ec) {
        fsr_candidates.push_back(cwd / "third_party" / "amd-fsr" / "signedbin");
    }
    for (const auto& candidate : fsr_candidates) {
        if (std::filesystem::exists(candidate / "amd_fidelityfx_loader_dx12.dll", ec) &&
            std::filesystem::exists(candidate / "amd_fidelityfx_upscaler_dx12.dll", ec)) {
            std::error_code abs_ec;
            const std::filesystem::path absolute =
                std::filesystem::absolute(candidate, abs_ec);
            info.fsr_dir = (abs_ec ? candidate : absolute).wstring();
            info.fsr_sr = true;
            info.fsr_fg = std::filesystem::exists(
                candidate / "amd_fidelityfx_framegeneration_dx12.dll", ec);
            break;
        }
    }
    return info;
}

std::vector<std::string> EnumerateAdapterNames() {
    std::vector<std::string> names;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return names;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            names.push_back(Narrow(desc.Description));
        }
    }
    return names;
}

}  // namespace brp::next
