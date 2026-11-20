#include "ai/dlss_fg.h"

#include "media/nv_dlssnr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace brp::next {
namespace {

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring text = path;
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

}  // namespace

DlssFg::DlssFg() = default;

DlssFg::~DlssFg() { Stop(); }

bool DlssFg::Start(const std::wstring& runtime_dir, int width, int height, int multiplier,
                   std::string& detail) {
    Stop();
    if (width <= 0 || height <= 0) {
        detail = "frame size is invalid";
        return false;
    }
    if (width > 2560 || height > 1440) {
        detail = "the DLSS-G worker is capped at 2560x1440";
        return false;
    }
    const std::filesystem::path dir(runtime_dir);
    const std::filesystem::path worker = dir / "nvngx.dll";
    if (!std::filesystem::exists(worker)) {
        detail = "nvngx.dll worker not found in " + dir.string();
        return false;
    }
    if (!std::filesystem::exists(dir / "nvngx_dlssg.dll")) {
        detail = "nvngx_dlssg.dll not found in " + dir.string();
        return false;
    }

    nr_ = std::make_unique<brp::media::NvDlssnr>();
    brp::media::NvDlssnr::Options options;
    options.style = 2;
    options.passes = 1;
    options.work_scale = 1.0f;
    if (!nr_->Start(worker.wstring(), dir.wstring(), width, height, options, detail)) {
        nr_.reset();
        return false;
    }
    nr_->SetNeuralBypass(true);
    multiplier_.store(std::clamp(multiplier, 2, 6));
    nr_->SetFrameGeneration(multiplier_.load());
    if (!nr_->EnablePresent(width, height, detail)) {
        nr_->Stop();
        nr_.reset();
        return false;
    }

    width_ = width;
    height_ = height;
    failed_.store(false);
    ready_ = true;
    visible_ = false;
    covering_ = false;
    worker_window_ = nullptr;
    last_x_ = last_y_ = last_w_ = last_h_ = 0;
    next_attach_ = 0.0;
    next_log_ = 0.0;
    displayed_fps_ = 0.0;
    actual_multiplier_ = 0;
    {
        std::lock_guard<std::mutex> lock(note_mutex_);
        note_ = L"DLSS-G " + std::to_wstring(multiplier_.load()) + L"x at " +
                std::to_wstring(width) + L"x" + std::to_wstring(height);
    }

    run_ = true;
    thread_ = std::thread([this]() { WorkerMain(); });
    std::fprintf(stderr, "[next] DLSS-G worker %dx%d %dx pid %lu\n", width, height,
                 multiplier_.load(),
                 static_cast<unsigned long>(nr_->worker_pid()));
    return true;
}

void DlssFg::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_ = false;
        pending_.reset();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (nr_) {
        nr_->SetFrameGeneration(0);
        nr_->DisablePresent();
        nr_->Stop();
    }
    nr_.reset();
    CloseWindow();
    ready_ = false;
    failed_.store(false);
    displayed_fps_ = 0.0;
    actual_multiplier_ = 0;
}

void DlssFg::SetMultiplier(int multiplier) {
    multiplier_.store(std::clamp(multiplier, 2, 6));
}

void DlssFg::Submit(std::vector<std::uint8_t> rgba, int width, int height) {
    if (!ready_ || rgba.empty() || width != width_ || height != height_) {
        return;
    }
    Job job;
    job.rgba = std::move(rgba);
    job.width = width;
    job.height = height;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            dropped_.fetch_add(1);
        }
        pending_ = std::move(job);
    }
    cv_.notify_one();
}

void DlssFg::WorkerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !run_ || pending_.has_value(); });
            if (!run_) {
                return;
            }
            job = std::move(*pending_);
            pending_.reset();
        }
        if (!nr_) {
            return;
        }
        nr_->SetFrameGeneration(multiplier_.load());
        std::vector<std::uint8_t> ignored;
        std::string detail;
        if (!nr_->Process(job.rgba.data(), job.width, job.height, ignored, detail)) {
            failed_.store(true);
            {
                std::lock_guard<std::mutex> lock(note_mutex_);
                note_ = L"DLSS-G: " + Widen(detail);
            }
            std::fprintf(stderr, "[next] DLSS-G process failed: %s\n", detail.c_str());
            return;
        }
    }
}

bool DlssFg::Attach(HWND app) {
    (void)app;
    if (worker_window_ && ::IsWindow(worker_window_)) {
        return true;
    }
    if (!nr_ || nr_->worker_pid() == 0) {
        return false;
    }
    struct Find {
        DWORD pid;
        HWND hwnd;
    } find{nr_->worker_pid(), nullptr};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* f = reinterpret_cast<Find*>(param);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            if (pid == f->pid && ::IsWindowVisible(hwnd) &&
                ::GetWindow(hwnd, GW_OWNER) == nullptr) {
                f->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (!find.hwnd) {
        return false;
    }
    // A topmost, never-activated overlay over the client. Reparenting it as a
    // child makes the worker believe it is occluded and it throttles itself.
    const LONG_PTR ex = ::GetWindowLongPtrW(find.hwnd, GWL_EXSTYLE);
    ::SetWindowLongPtrW(find.hwnd, GWL_EXSTYLE,
                        ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    worker_window_ = find.hwnd;
    last_x_ = last_y_ = last_w_ = last_h_ = 0;
    std::fprintf(stderr, "[next] DLSS-G window attached (hwnd %p)\n",
                 reinterpret_cast<void*>(worker_window_));
    return true;
}

void DlssFg::FrameWindow(HWND app, double now) {
    covering_ = false;
    if (!ready_ || !app) {
        return;
    }
    if (!worker_window_ && now < next_attach_) {
        return;
    }
    if (!Attach(app)) {
        next_attach_ = now + 0.5;
        return;
    }
    RECT client{};
    if (!::GetClientRect(app, &client)) {
        return;
    }
    POINT origin{0, 0};
    ::ClientToScreen(app, &origin);
    const int x = origin.x;
    const int y = origin.y;
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) {
        Hide();
        return;
    }

    // A topmost overlay must not cover other applications after Alt-Tab. Hide
    // it when neither our window nor the worker itself has focus.
    const HWND foreground = ::GetForegroundWindow();
    const bool focused = foreground == app || foreground == worker_window_;
    const bool show = focused;

    if (x != last_x_ || y != last_y_ || w != last_w_ || h != last_h_) {
        ::SetWindowPos(worker_window_, HWND_TOPMOST, x, y, w, h,
                       SWP_NOACTIVATE | SWP_NOOWNERZORDER | (show ? SWP_SHOWWINDOW : 0));
        last_x_ = x;
        last_y_ = y;
        last_w_ = w;
        last_h_ = h;
    }
    if (show != visible_) {
        ::ShowWindow(worker_window_, show ? SW_SHOW : SW_HIDE);
        visible_ = show;
    }
    covering_ = show;

    if (now >= next_log_) {
        next_log_ = now + 1.0;
        PollLog(now);
    }
}

void DlssFg::Hide() {
    if (worker_window_ && ::IsWindow(worker_window_)) {
        ::ShowWindow(worker_window_, SW_HIDE);
    }
    visible_ = false;
    covering_ = false;
}

void DlssFg::CloseWindow() {
    if (worker_window_ && ::IsWindow(worker_window_)) {
        ::ShowWindow(worker_window_, SW_HIDE);
    }
    worker_window_ = nullptr;
    visible_ = false;
    covering_ = false;
    last_x_ = last_y_ = last_w_ = last_h_ = 0;
}

void DlssFg::PollLog(double now) {
    const std::filesystem::path log = std::filesystem::path(ExeDirectory()) / L"nr-worker.log";
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(log, ec);
    if (ec || std::filesystem::file_time_type::clock::now() - modified >
                  std::chrono::seconds(3)) {
        displayed_fps_ = 0.0;
        return;
    }
    std::ifstream file(log, std::ios::binary);
    if (!file) {
        return;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    const std::streamoff start = size > 8192 ? size - 8192 : 0;
    file.seekg(start);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const std::size_t pos = text.rfind("[fg] displayed ");
    if (pos != std::string::npos) {
        double fps = 0.0;
        if (std::sscanf(text.c_str() + pos, "[fg] displayed %lf FPS", &fps) == 1) {
            displayed_fps_ = fps;
        }
        const std::size_t generated = text.find("generated, ", pos);
        if (generated != std::string::npos) {
            actual_multiplier_ = std::atoi(text.c_str() + generated + 11);
        }
    }
    (void)now;
}

int DlssFg::actual_multiplier() const { return actual_multiplier_; }

double DlssFg::displayed_fps() const { return displayed_fps_; }

DWORD DlssFg::worker_pid() const { return nr_ ? nr_->worker_pid() : 0; }

std::wstring DlssFg::note() const {
    std::lock_guard<std::mutex> lock(note_mutex_);
    return note_;
}

}  // namespace brp::next
