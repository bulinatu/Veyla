#include "streaming/streaming_service.h"

#include "diagnostics/log.h"
#include "session/discovery.h"
#include "session/psn_auth.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <thread>

namespace brp::next {
namespace {

const char* StateName(session::StreamState state) {
    switch (state) {
        case session::StreamState::Idle: return "idle";
        case session::StreamState::Connecting: return "connecting";
        case session::StreamState::Streaming: return "streaming";
        case session::StreamState::Failed: return "failed";
    }
    return "idle";
}

bool HostReachable(const std::string& host, int port) {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    bool reachable = false;
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) == 0) {
        for (addrinfo* entry = results; entry && !reachable; entry = entry->ai_next) {
            SOCKET socket = ::socket(entry->ai_family, entry->ai_socktype,
                                     entry->ai_protocol);
            if (socket == INVALID_SOCKET) {
                continue;
            }
            u_long nonblocking = 1;
            ::ioctlsocket(socket, FIONBIO, &nonblocking);
            const int result =
                ::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
            if (result == 0) {
                reachable = true;
            } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(socket, &writable);
                timeval timeout{};
                timeout.tv_usec = 400000;
                if (::select(0, nullptr, &writable, nullptr, &timeout) > 0) {
                    int error = 0;
                    int size = sizeof(error);
                    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                                     reinterpret_cast<char*>(&error), &size) == 0 &&
                        error == 0) {
                        reachable = true;
                    }
                }
            }
            ::closesocket(socket);
        }
        ::freeaddrinfo(results);
    }
    ::WSACleanup();
    return reachable;
}

}  // namespace

StreamingService::StreamingService(std::filesystem::path next_profile,
                                   std::filesystem::path legacy_profile)
    : next_path_(std::move(next_profile)), legacy_path_(std::move(legacy_profile)) {}

StreamingService::~StreamingService() {
    Disconnect();
    JoinDiscovery();
}

Status StreamingService::Load() {
    std::string detail;
    std::error_code ec;
    loaded_ = false;
    if (std::filesystem::exists(next_path_, ec)) {
        if (session::Load(next_path_.string(), profile_, detail)) {
            loaded_ = true;
            return Status::Ok();
        }
        LogWarning("[next] session load failed: " + detail);
    }
    if (std::filesystem::exists(legacy_path_, ec)) {
        if (session::Load(legacy_path_.string(), profile_, detail)) {
            loaded_ = true;
            // Own a copy so pairing and updates never touch the old app.
            const Status saved = SaveProfile();
            if (!saved.ok()) {
                LogWarning("[next] could not copy legacy session: " + saved.message());
            }
            return Status::Ok();
        }
        LogWarning("[next] legacy session load failed: " + detail);
    }
    profile_ = session::Profile{};
    return Status::Ok();
}

void StreamingService::SetStreamSettings(const StreamingSettings& settings) {
    stream_options_ = settings;
}

static int DecoderFromName(const std::string& name) {
    if (name == "d3d11va") return 1;
    if (name == "d3d12va") return 2;
    if (name == "vulkan") return 3;
    if (name == "dxva2") return 4;
    if (name == "cuda") return 5;
    if (name == "software") return 6;
    return 0;
}
Status StreamingService::Connect(const ConsoleSettings& console) {
    // A second request while the first session is still being established is
    // exactly what makes the console answer "Remote is already in use".
    if (streaming_.state() == session::StreamState::Streaming ||
        streaming_.state() == session::StreamState::Connecting) {
        return Status::Error("session is already active");
    }
    if (!loaded_ || !profile_.has_console()) {
        return Status::Error("no console saved; discover or pair one first");
    }
    if (profile_.needs_pairing()) {
        return Status::Error("console is not paired; enter the PIN first");
    }
    session::Profile profile = profile_;
    if (!console.address.empty()) {
        profile.host = console.address;
    }
    if (profile.host.empty()) {
        return Status::Error("console address is empty");
    }
    profile.fps = console.fps == 30 ? 30 : 60;
    profile.width = 1920;
    profile.height = 1080;
    profile.bitrate_kbps = std::clamp(stream_options_.bitrate_kbps, 1000, 200000);
    profile.decoder = DecoderFromName(stream_options_.decoder);
    profile.frame_buffer = std::clamp(stream_options_.frame_buffer, 0, 3);
    {
        char line[256] = {};
        std::snprintf(line, sizeof(line),
                      "stream: connect %s %dx%d@%d, %d kbps, decoder %s, buffer %d",
                      profile.host.c_str(), profile.width, profile.height, profile.fps,
                      profile.bitrate_kbps, stream_options_.decoder.c_str(),
                      profile.frame_buffer);
        LogInfo(line);
    }
    JoinConnect();
    streaming_.Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profile_.host = profile.host;
        profile_.decoder = profile.decoder;
        profile_.bitrate_kbps = profile.bitrate_kbps;
        profile_.width = profile.width;
        profile_.height = profile.height;
        message_ = "Connecting to " + profile.host;
        stats_ = {};
        last_stats_ = std::chrono::steady_clock::now();
        session_seconds_ = 0.0;
        session_frames_ = 0;
        session_lost_ = 0;
        session_drops_ = 0;
        session_kbps_sum_ = 0.0;
        session_fps_min_ = 0;
        session_fps_max_ = 0;
        session_fps_samples_ = 0;
        session_stalls_.store(0);
        stall_count_ = 0;
        advice_.clear();
    }
    connecting_.store(true);
    connect_thread_ = std::thread([this, profile]() {
        bool reachable = HostReachable(profile.host, 9295);
        if (!reachable) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                message_ = "Waking console at " + profile.host + "...";
            }
            const bool ps5 = profile.device_type != "PS4";
            for (int attempt = 0; attempt < 8 && !reachable; ++attempt) {
                session::WakeConsole(profile.host,
                                     profile.credentials.rp_regist_key.data(),
                                     profile.credentials.rp_regist_key.size(), ps5);
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                reachable = HostReachable(profile.host, 9295);
            }
        }
        if (!reachable) {
            std::lock_guard<std::mutex> lock(mutex_);
            connecting_.store(false);
            message_ = "Console is not reachable. Wake it or check the address.";
            LogWarning("stream: " + profile.host +
                       " is not reachable after the wake attempts");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "Starting session...";
        }
        std::string error;
        const bool ok =
            streaming_.Start(profile, profile.width, profile.height,
                            profile.bitrate_kbps, error);
        std::lock_guard<std::mutex> lock(mutex_);
        connecting_.store(false);
        if (!ok) {
            message_ = error.empty() ? "session start failed" : error;
            LogError("stream: start failed: " + message_);
        } else {
            message_ = "Waiting for the first frame";
            LogInfo("stream: session started, waiting for the first frame");
        }
    });
    return Status::Ok();
}

void StreamingService::Disconnect() {
    const bool was_active =
        streaming_.state() != session::StreamState::Idle || connecting_.load();
    JoinConnect();
    streaming_.Stop();
    std::string summary;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (was_active && session_seconds_ > 0.5 && session_frames_ > 0) {
            char line[360] = {};
            const double avg_fps =
                static_cast<double>(session_frames_) / session_seconds_;
            const double lost_pct =
                100.0 * static_cast<double>(session_lost_) /
                static_cast<double>(session_frames_);
            const double avg_mbps =
                session_kbps_sum_ / std::max(1.0, session_seconds_) / 1000.0;
            std::snprintf(
                line, sizeof(line),
                "stream: summary %.0fs, %lld frames, %.1f fps avg (min %d, max %d), "
                "%lld lost (%.2f%%), %lld drops, %d stalls, %.1f Mbps avg",
                session_seconds_, session_frames_, avg_fps, session_fps_min_,
                session_fps_max_, session_lost_, lost_pct, session_drops_,
                session_stalls_.load(), avg_mbps);
            summary = line;
        }
        message_ = "Disconnected";
        stats_ = {};
    }
    if (!summary.empty()) {
        LogInfo(summary);
    }
    if (was_active) {
        LogInfo("stream: disconnected");
    }
}

void StreamingService::JoinConnect() {
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }
    connecting_.store(false);
}

void StreamingService::JoinDiscovery() {
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
}

bool StreamingService::TakeLinkAdvice(std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (advice_.empty()) {
        return false;
    }
    text = advice_;
    advice_.clear();
    return true;
}

StreamSnapshot StreamingService::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamSnapshot snapshot;
    const session::StreamState state = streaming_.state();
    snapshot.state = connecting_.load() ? "connecting" : StateName(state);
    snapshot.connected = state == session::StreamState::Streaming;
    snapshot.streaming = snapshot.connected;
    snapshot.connecting = connecting_.load() ||
                          state == session::StreamState::Connecting;
    snapshot.pairing = pairing_.state() == session::PairState::Running;
    snapshot.discovering = discovering_.load();
    snapshot.has_credentials = profile_.credentials.present();
    snapshot.console_name = profile_.name;
    snapshot.console_address = profile_.host;
    snapshot.target_id = profile_.target_id;
    snapshot.codec = "hevc";
    switch (profile_.decoder) {
        case 1: snapshot.decoder = "D3D11VA"; break;
        case 2: snapshot.decoder = "D3D12VA"; break;
        case 3: snapshot.decoder = "Vulkan"; break;
        case 4: snapshot.decoder = "DXVA2"; break;
        case 5: snapshot.decoder = "CUDA"; break;
        case 6: snapshot.decoder = "Software"; break;
        default: snapshot.decoder = "Auto"; break;
    }
    snapshot.source_width = profile_.width;
    snapshot.source_height = profile_.height;
    snapshot.bitrate_kbps = static_cast<double>(profile_.bitrate_kbps);
    // fps() is frames over total uptime, so after a stop it decays instead of
    // reporting zero; an idle session has no source rate.
    snapshot.source_fps =
        snapshot.connected ? streaming_.fps() : 0.0;
    snapshot.decode_ms = stats_.decode_ms_avg;
    snapshot.decode_ms_max = stats_.decode_ms_max;
    snapshot.decoder_drops = stats_.buffer_drops;
    snapshot.jitter_ms = stats_.arrival_ms;
    if (stats_.frames > 0) {
        snapshot.loss_percent =
            100.0 * static_cast<double>(stats_.lost) / static_cast<double>(stats_.frames);
    }
    snapshot.message = streaming_.message();
    if (snapshot.message.empty()) {
        snapshot.message = message_;
    }
    snapshot.pairing_message = pairing_message_;
    return snapshot;
}

Status StreamingService::Pair(std::uint32_t pin) {
    if (!profile_.has_console()) {
        return Status::Error("set the console address first");
    }
    if (pin < 10000000u || pin > 99999999u) {
        return Status::Error("the PIN must be 8 digits");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pairing_.state() == session::PairState::Running) {
        return Status::Error("pairing is already running");
    }
    if (!pairing_.Start(profile_, pin)) {
        return Status::Error("could not start pairing");
    }
    pairing_message_ = "Pairing...";
    return Status::Ok();
}

Status StreamingService::StartDiscovery() {
    if (discovering_.load()) {
        return Status::Error("discovery is already running");
    }
    JoinDiscovery();
    discovering_.store(true);
    discovery_thread_ = std::thread([this]() {
        const auto found = session::DiscoverLocal(1500);
        std::vector<ConsoleCandidate> candidates;
        candidates.reserve(found.size());
        for (const auto& console : found) {
            ConsoleCandidate candidate;
            candidate.address = console.address;
            candidate.name = console.name;
            candidate.target_id = console.target_id;
            candidate.device_type = console.device_type;
            candidate.status = console.status;
            candidates.push_back(std::move(candidate));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        discovered_ = std::move(candidates);
        discovering_.store(false);
    });
    return Status::Ok();
}

std::vector<ConsoleCandidate> StreamingService::Discovered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discovered_;
}

bool StreamingService::HasCredentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profile_.credentials.present();
}

void StreamingService::Poll() {
    pairing_.Poll();
    const session::PairState pair_state = pairing_.state();
    if (pair_state == session::PairState::Success) {
        session::Credentials credentials;
        if (pairing_.TakeCredentials(credentials)) {
            std::lock_guard<std::mutex> lock(mutex_);
            profile_.credentials = credentials;
            pairing_message_ = "Paired successfully";
            const Status saved = SaveProfile();
            if (!saved.ok()) {
                pairing_message_ += ": " + saved.message();
            }
        }
    } else if (pair_state == session::PairState::Failed) {
        std::lock_guard<std::mutex> lock(mutex_);
        pairing_message_ = pairing_.message();
    } else if (pair_state == session::PairState::Running) {
        std::lock_guard<std::mutex> lock(mutex_);
        pairing_message_ = "Pairing...";
    }

    const auto now = std::chrono::steady_clock::now();
    if (streaming_.state() == session::StreamState::Streaming &&
        now - last_stats_ > std::chrono::seconds(1)) {
        const session::Streaming::StreamStats stats = streaming_.ConsumeStats();
        const double fps = stats.elapsed > 0.0 ? stats.frames / stats.elapsed : 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_ = stats;
            last_stats_ = now;
            session_seconds_ += stats.elapsed;
            session_frames_ += stats.frames;
            session_lost_ += stats.lost;
            session_drops_ += stats.buffer_drops;
            session_kbps_sum_ += stats.kbps;
            if (stats.frames > 0) {
                const int fps_i = static_cast<int>(fps + 0.5);
                if (session_fps_samples_ == 0 || fps_i < session_fps_min_) {
                    session_fps_min_ = fps_i;
                }
                if (fps_i > session_fps_max_) {
                    session_fps_max_ = fps_i;
                }
                ++session_fps_samples_;
            }
            if (message_ == "Waiting for the first frame" && stats.frames > 0) {
                message_ = "Streaming";
            }
        }
        // One line per second at Debug: source cadence, decoder time and every
        // drop counter the presentation path relies on. A degraded second is
        // repeated at Warning (throttled) so it is visible on the default level
        // too.
        // Do not even format the per-second line in production.
        if (CurrentLogLevel() <= LogLevel::Debug) {
            const double lost_pct =
                stats.frames > 0
                    ? 100.0 * static_cast<double>(stats.lost) / stats.frames
                    : 0.0;
            char line[320] = {};
            std::snprintf(
                line, sizeof(line),
                "stream: %.1f fps, lost %d (%.2f%%), decode %.1f/%.1f ms, queue %d, "
                "buffer %d, drops %d, arrival %.1f ms, outliers %d, %.0f kbps",
                fps, stats.lost, lost_pct, stats.decode_ms_avg, stats.decode_ms_max,
                stats.queue, stats.buffer, stats.buffer_drops, stats.arrival_ms,
                stats.arrival_outliers, stats.kbps);
            LogDebug(line);
        }
        if (stats.lost > 0) {
            streaming_.NoteLoss();
        }
        // Outliers are normal on Wi-Fi and the buffer absorbs them; warning on
        // every burst made the log look broken while the picture was fine. Warn
        // only when frames were actually lost/dropped, decode stalled, or the
        // source rate collapsed.
        // A stall with a clean link (no loss, fast decode) is the signature of
        // Wi-Fi power save / roaming / MLO or a console encoder pause. Count it
        // and, after it repeats, tell the shell what to change.
        // Skip the priming seconds at session start; they always look slow.
        const bool stall = session_seconds_ > 5.0 && stats.elapsed > 0.5 &&
                           fps < 24.0 && stats.lost == 0 &&
                           stats.decode_ms_max < 30.0;
        if (stall) {
            ++stall_count_;
            session_stalls_.fetch_add(1);
            if (stall_count_ == 1) {
                first_stall_ = now;
            }
            char stall_line[260] = {};
            std::snprintf(stall_line, sizeof(stall_line),
                          "stream: stall #%d (%.1f fps, queue %d, buffer %d, "
                          "%.0f kbps, decode %.1f ms)",
                          stall_count_, fps, stats.queue, stats.buffer, stats.kbps,
                          stats.decode_ms_max);
            LogWarning(stall_line);
            if (stall_count_ >= 2) {
                streaming_.NoteLoss();
            }
            if (stall_count_ == 3) {
                const std::string advice =
                    "Wi-Fi stalls with a clean ping: set the adapter to maximum "
                    "performance, disable Wi-Fi 7 MLO and roaming scans, and update "
                    "the Wi-Fi driver.";
                LogWarning("stream: diagnosis: " + advice);
                std::lock_guard<std::mutex> lock(mutex_);
                advice_ = advice;
            }
        } else if (stall_count_ > 0 &&
                   now - first_stall_ > std::chrono::minutes(5)) {
            stall_count_ = 0;
        }
        const bool degraded =
            stats.lost > 0 || stats.buffer_drops > 0 || stats.decode_ms_max > 40.0 ||
            (stats.elapsed > 0.5 && fps < stream_options_.fps * 0.75);
        if (degraded && now - last_warn_ > std::chrono::seconds(5)) {
            last_warn_ = now;
            char warning[260] = {};
            std::snprintf(warning, sizeof(warning),
                          "stream: degraded second (%.1f fps, lost %d, drops %d, "
                          "decode %.1f ms, %.0f kbps)",
                          fps, stats.lost, stats.buffer_drops, stats.decode_ms_max,
                          stats.kbps);
            LogWarning(warning);
        }
    }
}

Status StreamingService::SetConsole(const ConsoleCandidate& candidate) {
    if (candidate.address.empty()) {
        return Status::Error("console address is empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    profile_.host = candidate.address;
    if (!candidate.name.empty()) {
        profile_.name = candidate.name;
    }
    if (!candidate.target_id.empty()) {
        profile_.target_id = candidate.target_id;
    }
    if (!candidate.device_type.empty()) {
        profile_.device_type = candidate.device_type;
    }
    return SaveProfile();
}

Status StreamingService::SaveProfile() {
    std::error_code ec;
    if (next_path_.has_parent_path()) {
        std::filesystem::create_directories(next_path_.parent_path(), ec);
    }
    std::string detail;
    if (!session::Save(profile_, next_path_.string(), detail)) {
        return Status::Error("cannot save session: " + detail);
    }
    return Status::Ok();
}

Status StreamingService::OpenPsnLogin(std::string& detail) {
    if (!session::OpenPsnLoginPage(detail)) {
        return Status::Error(detail);
    }
    return Status::Ok();
}

Status StreamingService::CompletePsnLogin(const std::string& redirect, std::string& detail) {
    session::PsnAccount account;
    if (!session::PsnCompleteLogin(redirect, account, detail)) {
        return Status::Error(detail);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    profile_.psn_account_id = account.account_id;
    profile_.psn_numeric_id = account.numeric_id;
    const Status saved = SaveProfile();
    if (!saved.ok()) {
        return saved;
    }
    return Status::Ok();
}

void StreamingService::SetAudio(brp::audio::Engine* engine) {
    streaming_.SetAudio(engine);
}

void StreamingService::SetMotionEnabled(bool enabled) {
    streaming_.SetMotionEnabled(enabled);
}

bool StreamingService::TakeFrame(media::DecodedFrame& out, int buffer, bool paced) {
    return streaming_.TakeFrame(out, buffer, paced);
}

bool StreamingService::TakeNewestFrame(media::DecodedFrame& out) {
    return streaming_.TakeNewestFrame(out);
}

bool StreamingService::TakeFrameOrdered(media::DecodedFrame& out, int max_queue) {
    return streaming_.TakeFrameOrdered(out, max_queue);
}

bool StreamingService::WaitForFrame(int timeout_ms) {
    return streaming_.WaitForFrame(timeout_ms);
}

int StreamingService::QueueDepth() {
    return streaming_.queue_depth();
}

int StreamingService::BufferTarget() const {
    return streaming_.buffer_target();
}

void StreamingService::SendController(const brp::PadState& state) {
    streaming_.SendController(state);
}

brp::PadConsoleFeedback StreamingService::Feedback() const {
    return streaming_.feedback();
}

bool StreamingService::TakeHaptics(std::uint16_t& left, std::uint16_t& right) {
    return streaming_.TakeHaptics(left, right);
}

bool StreamingService::Connected() const {
    return streaming_.state() == session::StreamState::Streaming;
}
session::Profile StreamingService::Profile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profile_;
}

}  // namespace brp::next
