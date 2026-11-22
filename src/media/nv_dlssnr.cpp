#include "media/nv_dlssnr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace brp::media {
namespace {

constexpr uint32_t kVideoMagic = 0x33563544;      // DV5
constexpr uint32_t kResizeMagic = 0x5A534E52;     // RNSZ
constexpr uint32_t kResizeAckMagic = 0x4B434152;  // RACK
constexpr uint32_t kCreateAckMagic = 0x4B434143;  // CACK
constexpr uint32_t kShmMagic = 0x494D4853;        // SHMI
constexpr uint32_t kShmAckMagic = 0x4B434153;     // SACK
constexpr uint32_t kOutsMagic = 0x5354554F;       // OUTS
constexpr uint32_t kOutsAckMagic = 0x324B414F;    // OAK2
constexpr uint32_t kWindowMagic = 0x4F444E57;     // WNDO
constexpr uint32_t kWindowAckMagic = 0x4B434157;  // WACK
constexpr uint32_t kWindowFlagCapturable = 0x1;
constexpr uint32_t kFrameMagic = 0x314D5246;      // FMR1
constexpr uint32_t kOutMagic = 0x3154554F;        // OUT1
constexpr uint32_t kFrameFlagShm = 0x1;
constexpr uint32_t kFrameFlagWantPixels = 0x2;
constexpr uint32_t kOutBytesInShm = 0xFFFFFFFF;  // OUT1 byte_count sentinel
// RNSZ flags word (the same 4-byte slot the VIDEO header uses for frame_count).
constexpr uint32_t kResizeFlagNrSmall = 0x1;  // network at work size, upscaled back
constexpr uint32_t kResizePassesShift = 2;
constexpr int kMaxPasses = 4;

#pragma pack(push, 1)
struct VideoHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t warmup;
    uint32_t frame_count;
    uint32_t profile;
    uint32_t preset;
    uint32_t style;
    uint32_t auto_mask;
    uint32_t ui_correction;
    float intensity;
    float local_tone;
    float local_structure;
    float skin_structure;
    uint32_t full_w;
    uint32_t full_h;
};
struct ShmCommand {
    uint32_t magic;
    uint32_t color_bytes;
    uint32_t motion_bytes;
    uint32_t flags;
    int64_t pts;
    char name[64];
};
struct AckHeader {
    uint32_t magic;
    uint32_t ok;
    uint32_t r0;
    uint32_t r1;
    int64_t pts;
};
struct FrameCommand {
    uint32_t magic;
    uint32_t index;
    uint32_t reset;
    uint32_t flags;
    int64_t pts;
};
struct OutHeader {
    uint32_t magic;
    uint32_t index;
    uint32_t ok;
    uint32_t bytes;
    uint32_t ngx_result;
    int64_t pts;
};
#pragma pack(pop)

static_assert(sizeof(VideoHeader) == 64, "VIDEO header must stay 64 bytes");
static_assert(sizeof(ShmCommand) == 88, "SHMI/OUTS must stay 88 bytes");
static_assert(sizeof(AckHeader) == 24, "ack header must stay 24 bytes");
static_assert(sizeof(FrameCommand) == 24, "FRAME header must stay 24 bytes");
static_assert(sizeof(OutHeader) == 28, "OUT header must stay 28 bytes");

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// The NGX work size. A downscale rounds DOWN to an even number and never
// exceeds the frame; 1.0 keeps the frame exactly (odd sizes stay on the 1:1
// path, which is the known-good one).
void WorkSize(int full_w, int full_h, float scale, int* work_w, int* work_h) {
    if (scale >= 1.0f) {
        *work_w = full_w;
        *work_h = full_h;
        return;
    }
    *work_w = std::max(64, static_cast<int>(full_w * scale) / 2 * 2);
    *work_h = std::max(64, static_cast<int>(full_h * scale) / 2 * 2);
    *work_w = std::min(*work_w, full_w);
    *work_h = std::min(*work_h, full_h);
}

}  // namespace

namespace {

bool InjectProxy(HANDLE process, const std::wstring& dll_path, std::string& detail) {
    const std::wstring path = std::filesystem::absolute(dll_path).wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!remote) {
        detail = "proxy alloc failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    bool ok = ::WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr) != FALSE;
    if (ok) {
        const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
        const auto load =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(kernel, "LoadLibraryW"));
        HANDLE thread = load ? ::CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr)
                             : nullptr;
        if (!thread) {
            ok = false;
            detail = "proxy thread failed (" + std::to_string(::GetLastError()) + ")";
        } else {
            ::WaitForSingleObject(thread, 10000);
            DWORD result = 0;
            ::GetExitCodeThread(thread, &result);
            ::CloseHandle(thread);
            ok = result != 0;
            if (!ok) {
                detail = "proxy LoadLibrary returned 0";
            }
        }
    } else {
        detail = "proxy write failed (" + std::to_string(::GetLastError()) + ")";
    }
    ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

}  // namespace
NvDlssnr::~NvDlssnr() {
    Stop();
}

void NvDlssnr::CloseHandles() {
    if (in_view_) {
        ::UnmapViewOfFile(in_view_);
        in_view_ = nullptr;
    }
    if (out_view_) {
        ::UnmapViewOfFile(out_view_);
        out_view_ = nullptr;
    }
    if (in_map_) {
        ::CloseHandle(in_map_);
        in_map_ = nullptr;
    }
    if (out_map_) {
        ::CloseHandle(out_map_);
        out_map_ = nullptr;
    }
    if (stdin_write_) {
        ::CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (stdout_read_) {
        ::CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (log_file_) {
        ::CloseHandle(log_file_);
        log_file_ = nullptr;
    }
    if (process_) {
        ::CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_) {
        ::CloseHandle(job_);
        job_ = nullptr;
    }
}

void NvDlssnr::TerminateWorker() {
    if (!process_) {
        return;
    }
    if (job_) {
        ::TerminateJobObject(job_, 1);
    } else {
        ::TerminateProcess(process_, 1);
    }
}

void NvDlssnr::Stop() {
    pending_ = false;
    if (stdin_write_) {
        ::CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (process_) {
        // A live worker can be inside a long present/NGX call. It has no state
        // worth saving, so give it a short grace period and then terminate.
        if (::WaitForSingleObject(process_, 100) != WAIT_OBJECT_0) {
            if (job_) {
                ::TerminateJobObject(job_, 1);
            } else {
                ::TerminateProcess(process_, 1);
            }
            ::WaitForSingleObject(process_, 1000);
        }
    }
    CloseHandles();
    ready_ = false;
    present_mode_ = false;
    worker_pid_ = 0;
}

bool NvDlssnr::WriteAll(const void* data, size_t bytes, std::string& detail) {
    const auto* bytes_data = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < bytes) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes - offset, 1u << 20));
        if (!::WriteFile(stdin_write_, bytes_data + offset, chunk, &written, nullptr) ||
            written == 0) {
            detail = "NR worker write failed (" + std::to_string(::GetLastError()) + ")";
            return false;
        }
        offset += written;
    }
    return true;
}

bool NvDlssnr::ReadExact(void* data, size_t bytes, DWORD timeout_ms, std::string& detail) {
    auto* out = static_cast<uint8_t*>(data);
    const double deadline = NowMs() + timeout_ms;
    size_t offset = 0;
    while (offset < bytes) {
        DWORD available = 0;
        if (!::PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr)) {
            detail = "NR worker pipe closed (" + std::to_string(::GetLastError()) + ")";
            return false;
        }
        if (available > 0) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes - offset, available));
            if (!::ReadFile(stdout_read_, out + offset, chunk, &read, nullptr) || read == 0) {
                detail = "NR worker read failed (" + std::to_string(::GetLastError()) + ")";
                return false;
            }
            offset += read;
            continue;
        }
        if (NowMs() > deadline) {
            detail = "NR worker timed out";
            return false;
        }
        // Wait on the pipe (signaled when bytes are there) and on the process,
        // so a terminated worker wakes this loop immediately instead of
        // spinning until the timeout.
        HANDLE waits[2] = {stdout_read_, process_};
        const DWORD count = process_ ? 2 : 1;
        const DWORD wait = ::WaitForMultipleObjects(count, waits, FALSE, 1);
        if (process_ && wait == WAIT_OBJECT_0 + 1) {
            detail = "NR worker exited";
            return false;
        }
    }
    return true;
}

bool NvDlssnr::WaitForMagic(uint32_t expected, uint32_t* extra, std::string& detail) {
    uint32_t magic = 0;
    if (!ReadExact(&magic, sizeof(magic), 60000, detail)) {
        return false;
    }
    if (magic != expected) {
        detail = "NR worker unexpected reply " + Hex(magic) + ", wanted " + Hex(expected);
        return false;
    }
    AckHeader ack{};
    if (!ReadExact(reinterpret_cast<uint8_t*>(&ack) + 4, sizeof(ack) - 4, 60000, detail)) {
        return false;
    }
    ack.magic = magic;
    if (extra) {
        *extra = ack.ok;
    }
    if (ack.ok == 0) {
        detail = "NR worker refused " + Hex(expected);
        return false;
    }
    return true;
}

bool NvDlssnr::Start(const std::wstring& worker_exe, const std::wstring& working_dir, int width,
                     int height, const Options& options, std::string& detail) {
    Stop();
    if (width <= 0 || height <= 0) {
        detail = "NR size is invalid";
        return false;
    }
    if (!std::filesystem::exists(worker_exe)) {
        detail = "NR worker not found: " + std::filesystem::path(worker_exe).string();
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!::CreatePipe(&stdin_read, &stdin_write_, &sa, 1 << 20) ||
        !::CreatePipe(&stdout_read_, &stdout_write, &sa, 1 << 20)) {
        detail = "cannot create NR pipes";
        if (stdin_read) {
            ::CloseHandle(stdin_read);
        }
        if (stdout_write) {
            ::CloseHandle(stdout_write);
        }
        CloseHandles();
        return false;
    }
    ::SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    // The worker logs once per second. A growing text file on the system drive
    // can be scanned by antivirus or flushed at exactly that cadence, which
    // shows up as one held frame per second. Keep the worker quiet by default;
    // BRP_FG_LOG=1 restores nr-worker.log for diagnostics.
    const bool want_worker_log = std::getenv("BRP_FG_LOG") != nullptr;
    if (want_worker_log) {
        log_file_ = ::CreateFileW((ExeDirectory() + L"\\nr-worker.log").c_str(),
                                  GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (log_file_ == INVALID_HANDLE_VALUE || !log_file_) {
        // Never let the worker's log fall through to stdout: that pipe is the
        // protocol, and a log line there desynchronises every reply. NUL is a
        // real sink and keeps the worker's per-second write off the disk.
        log_file_ = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_file_ == INVALID_HANDLE_VALUE) {
            log_file_ = nullptr;
        }
    }

    if (!log_file_) {
        detail = "cannot open the NR worker log (and NUL is unavailable)";
        ::CloseHandle(stdin_read);
        ::CloseHandle(stdout_write);
        CloseHandles();
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = log_file_;

    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + worker_exe + L"\" --live";
    const BOOL created = ::CreateProcessW(worker_exe.c_str(), command.data(), nullptr, nullptr,
                                          TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                          working_dir.c_str(), &si, &pi);
    ::CloseHandle(stdin_read);
    ::CloseHandle(stdout_write);
    if (!created) {
        detail = "cannot start NR worker (" + std::to_string(::GetLastError()) + ")";
        CloseHandles();
        return false;
    }
    process_ = pi.hProcess;
    worker_pid_ = pi.dwProcessId;

    job_ = ::CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        ::AssignProcessToJobObject(job_, process_);
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    // Optional DLSSG-SM86 proxy (user-supplied): it raises the Frame
    // Generation multiplier ceiling on non-RTX-50 cards. Injection happens
    // right after the process starts, long before the FG runtime is loaded.
    const std::wstring proxy = working_dir + L"\\fg\\winmm.dll";
    if (options.inject_fg_proxy && std::filesystem::exists(proxy)) {
        std::string proxy_detail;
        if (InjectProxy(process_, proxy, proxy_detail)) {
            std::fprintf(stderr, "[nr] FG proxy injected\n");
        } else {
            std::fprintf(stderr, "[nr] FG proxy not injected: %s\n", proxy_detail.c_str());
        }
    }

    int work_w = width;
    int work_h = height;
    WorkSize(width, height, options.work_scale, &work_w, &work_h);

    VideoHeader header{};
    header.magic = kVideoMagic;
    header.width = static_cast<uint32_t>(work_w);
    header.height = static_cast<uint32_t>(work_h);
    header.warmup = 0;
    header.frame_count = 0;  // 0 = endless loop (flags word only in RNSZ)
    header.profile = 0;
    header.preset = 0;
    header.style = static_cast<uint32_t>(options.style);
    header.auto_mask = options.auto_mask ? 1u : 0u;
    header.ui_correction = options.ui_correction ? 1u : 0u;
    header.intensity = options.intensity;
    header.local_tone = options.local_tone;
    header.local_structure = options.local_structure;
    header.skin_structure = options.skin_structure;
    header.full_w = static_cast<uint32_t>(width);
    header.full_h = static_cast<uint32_t>(height);
    if (!WriteAll(&header, sizeof(header), detail)) {
        Stop();
        return false;
    }
    if (!WaitForMagic(kCreateAckMagic, nullptr, detail)) {
        Stop();
        return false;
    }

    const uint64_t color_bytes = static_cast<uint64_t>(width) * height * 4ull;
    const uint64_t motion_bytes = 2560ull * 1440ull * 4ull;
    in_size_ = static_cast<size_t>(color_bytes + motion_bytes);
    const std::wstring in_name =
        L"NeuralScreen_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(::GetTickCount64());
    in_map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                   static_cast<DWORD>(in_size_ >> 32),
                                   static_cast<DWORD>(in_size_ & 0xFFFFFFFF), in_name.c_str());
    if (!in_map_) {
        detail = "cannot create NR input mapping";
        Stop();
        return false;
    }
    in_view_ = static_cast<uint8_t*>(::MapViewOfFile(in_map_, FILE_MAP_ALL_ACCESS, 0, 0, in_size_));
    if (!in_view_) {
        detail = "cannot map NR input";
        Stop();
        return false;
    }
    std::memset(in_view_, 0, in_size_);

    ShmCommand shm{};
    shm.magic = kShmMagic;
    shm.color_bytes = static_cast<uint32_t>(color_bytes);
    shm.motion_bytes = static_cast<uint32_t>(motion_bytes);
    std::snprintf(shm.name, sizeof(shm.name), "%ls", in_name.c_str());
    if (!WriteAll(&shm, sizeof(shm), detail) ||
        !WaitForMagic(kShmAckMagic, nullptr, detail)) {
        Stop();
        return false;
    }

    out_size_ = static_cast<size_t>(color_bytes + 8);
    const std::wstring out_name =
        L"NeuralScreenOut_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(::GetTickCount64() + 1);
    out_map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                    static_cast<DWORD>(out_size_ >> 32),
                                    static_cast<DWORD>(out_size_ & 0xFFFFFFFF),
                                    out_name.c_str());
    if (!out_map_) {
        detail = "cannot create NR output mapping";
        Stop();
        return false;
    }
    out_view_ =
        static_cast<uint8_t*>(::MapViewOfFile(out_map_, FILE_MAP_ALL_ACCESS, 0, 0, out_size_));
    if (!out_view_) {
        detail = "cannot map NR output";
        Stop();
        return false;
    }
    std::memset(out_view_, 0, out_size_);

    ShmCommand outs{};
    outs.magic = kOutsMagic;
    outs.color_bytes = static_cast<uint32_t>(width);
    outs.motion_bytes = static_cast<uint32_t>(height);
    std::snprintf(outs.name, sizeof(outs.name), "%ls", out_name.c_str());
    if (!WriteAll(&outs, sizeof(outs), detail) ||
        !WaitForMagic(kOutsAckMagic, nullptr, detail)) {
        Stop();
        return false;
    }

    options_ = options;
    full_w_ = width;
    full_h_ = height;
    work_w_ = work_w;
    work_h_ = work_h;
    frames_ = 0;
    frame_index_ = 0;
    ready_ = true;
    status_ = "NR ready";

    // The VIDEO header has no flags word (that slot is frame_count), so Boost
    // and a pass count > 1 travel in an RNSZ right after the channel setup -
    // the same order the reference client uses.
    if (work_w != full_w_ || work_h != full_h_ || options_.passes > 1) {
        if (!SendResize(options_, work_w_, work_h_, detail)) {
            Stop();
            return false;
        }
    }
    return true;
}

bool NvDlssnr::EnablePresent(int width, int height, std::string& detail) {
    if (present_mode_) {
        return true;
    }
    if (!ready_ || !stdin_write_) {
        detail = "NR is not running";
        return false;
    }
    struct WindowCommand {
        uint32_t magic;
        uint32_t width;
        uint32_t height;
        uint32_t flags;
        int64_t pts;
    };
    static_assert(sizeof(WindowCommand) == 24, "WNDO must stay 24 bytes");
    WindowCommand cmd{};
    cmd.magic = kWindowMagic;
    cmd.width = static_cast<uint32_t>(width);
    cmd.height = static_cast<uint32_t>(height);
    cmd.flags = kWindowFlagCapturable;
    if (!WriteAll(&cmd, sizeof(cmd), detail) ||
        !WaitForMagic(kWindowAckMagic, nullptr, detail)) {
        return false;
    }
    present_mode_ = true;
    return true;
}

void NvDlssnr::DisablePresent() {
    if (!ready_ || !stdin_write_ || !present_mode_) {
        return;
    }
    struct WindowCommand {
        uint32_t magic;
        uint32_t width;
        uint32_t height;
        uint32_t flags;
        int64_t pts;
    };
    WindowCommand cmd{};
    cmd.magic = kWindowMagic;
    cmd.flags = 0x2;  // WINDOW_FLAG_DISABLE
    std::string ignored;
    WriteAll(&cmd, sizeof(cmd), ignored);
    present_mode_ = false;
}
bool NvDlssnr::SendResize(const Options& options, int work_w, int work_h,
                          std::string& detail) {
    VideoHeader cmd{};
    cmd.magic = kResizeMagic;
    cmd.width = static_cast<uint32_t>(work_w);
    cmd.height = static_cast<uint32_t>(work_h);
    cmd.warmup = 0;
    const int passes = std::clamp(options.passes, 1, kMaxPasses);
    cmd.frame_count = (work_w != full_w_ || work_h != full_h_ ? kResizeFlagNrSmall : 0u) |
                      (static_cast<uint32_t>(passes) << kResizePassesShift);
    cmd.profile = 0;
    cmd.preset = 0;
    cmd.style = static_cast<uint32_t>(options.style);
    cmd.auto_mask = options.auto_mask ? 1u : 0u;
    cmd.ui_correction = options.ui_correction ? 1u : 0u;
    cmd.intensity = options.intensity;
    cmd.local_tone = options.local_tone;
    cmd.local_structure = options.local_structure;
    cmd.skin_structure = options.skin_structure;
    cmd.full_w = static_cast<uint32_t>(full_w_);
    cmd.full_h = static_cast<uint32_t>(full_h_);
    if (!WriteAll(&cmd, sizeof(cmd), detail)) {
        return false;
    }
    const double started = NowMs();
    if (!WaitForMagic(kResizeAckMagic, nullptr, detail)) {
        return false;
    }
    status_ = "NR reconfigured";
    std::fprintf(stderr, "[nr] RNSZ %dx%d (full %dx%d, %d pass, %.0f ms)\n", work_w, work_h,
                 full_w_, full_h_, passes, NowMs() - started);
    return true;
}

bool NvDlssnr::Reconfigure(const Options& options, std::string& detail) {
    if (!ready_) {
        detail = "NR is not running";
        return false;
    }
    int work_w = full_w_;
    int work_h = full_h_;
    WorkSize(full_w_, full_h_, options.work_scale, &work_w, &work_h);
    if (!SendResize(options, work_w, work_h, detail)) {
        return false;
    }
    options_ = options;
    work_w_ = work_w;
    work_h_ = work_h;
    return true;
}

bool NvDlssnr::Submit(const uint8_t* rgba, int width, int height, std::string& detail) {
    if (!ready_ || !rgba || width != full_w_ || height != full_h_ || !in_view_ || !out_view_) {
        detail = "NR is not initialized for this frame size";
        return false;
    }
    if (pending_) {
        detail = "NR already has a frame in flight";
        return false;
    }
    const size_t color_bytes = static_cast<size_t>(width) * height * 4;
    submitted_at_ = NowMs();
    std::memcpy(in_view_, rgba, color_bytes);

    FrameCommand frame{};
    frame.magic = kFrameMagic;
    frame.index = frame_index_;
    frame.reset = frame_index_ == 0 ? 1u : 0u;
    uint32_t flags = kFrameFlagShm | (present_mode_ ? 0u : kFrameFlagWantPixels);
    if (nr_bypass_) {
        flags |= 0x10u;  // FRAME_FLAG_BYPASS
    }
    if (fg_multiplier_ >= 2) {
        // Bits 8-11: FG enabled, multiplier minus two, explicit-UI flag; bit
        // 15 carries the third multiplier bit.
        flags |= 0x800u | 0x100u;
        const uint32_t step = static_cast<uint32_t>(std::clamp(fg_multiplier_, 2, 6) - 2);
        flags |= (step & 3u) << 9;
        if (step & 4u) {
            flags |= 0x8000u;  // FRAME_FLAG_FG_MULT_HIGH
        }
    }
    frame.flags = flags;
    frame.pts = static_cast<int64_t>(frame_index_);
    ++frame_index_;
    if (!WriteAll(&frame, sizeof(frame), detail)) {
        ready_ = false;
        return false;
    }
    pending_ = true;
    return true;
}

bool NvDlssnr::Collect(std::vector<uint8_t>& out, std::string& detail) {
    if (!pending_) {
        detail = "NR has no frame in flight";
        return false;
    }
    pending_ = false;
    const size_t color_bytes = static_cast<size_t>(full_w_) * full_h_ * 4;

    uint32_t magic = 0;
    if (!ReadExact(&magic, sizeof(magic), 4000, detail)) {
        ready_ = false;
        return false;
    }
    if (magic != kOutMagic) {
        detail = "NR worker unexpected frame reply " + Hex(magic);
        ready_ = false;
        return false;
    }
    OutHeader reply{};
    if (!ReadExact(reinterpret_cast<uint8_t*>(&reply) + 4, sizeof(reply) - 4, 4000, detail)) {
        ready_ = false;
        return false;
    }
    reply.magic = magic;
    if (reply.ok == 0) {
        detail = "NR worker failed frame (ngx " + Hex(reply.ngx_result) + ")";
        ready_ = false;
        return false;
    }
    if (present_mode_) {
        // WNDO: the worker shows the frame (and generated ones) itself.
        last_ms_ = NowMs() - submitted_at_;
        ++frames_;
        return true;
    }
    if (reply.bytes != kOutBytesInShm) {
        detail = "NR worker returned " + std::to_string(reply.bytes) +
                 " bytes instead of a shared-memory frame";
        ready_ = false;
        return false;
    }

    out.resize(color_bytes);
    bool copied = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint64_t seq1 = 0;
        std::memcpy(&seq1, out_view_, sizeof(seq1));
        if (seq1 & 1) {
            ::SwitchToThread();
            continue;
        }
        std::memcpy(out.data(), out_view_ + 8, color_bytes);
        uint64_t seq2 = 0;
        std::memcpy(&seq2, out_view_, sizeof(seq2));
        if (seq1 == seq2) {
            copied = true;
            break;
        }
    }
    if (!copied) {
        detail = "NR output seqlock remained unstable";
        ready_ = false;
        return false;
    }

    last_ms_ = NowMs() - submitted_at_;
    ++frames_;
    status_ = "NR running";
    return true;
}

bool NvDlssnr::Process(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& out,
                       std::string& detail) {
    if (pending_ && !Collect(out, detail)) {
        return false;
    }
    if (!Submit(rgba, width, height, detail)) {
        return false;
    }
    return Collect(out, detail);
}

int NvDlssnr::ProcessPipelined(const uint8_t* rgba, int width, int height,
                               std::vector<uint8_t>& out, std::string& detail) {
    if (!pending_) {
        // First frame: nothing is in flight yet, so there is no output to return.
        return Submit(rgba, width, height, detail) ? 0 : -1;
    }
    // The worker computed the previous frame while the loop did the rest of
    // its work, so this collect does not wait. Then the current frame goes out.
    if (!Collect(out, detail)) {
        return -1;
    }
    return Submit(rgba, width, height, detail) ? 1 : -1;
}

}  // namespace brp::media
