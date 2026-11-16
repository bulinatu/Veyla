#include "session/pairing.h"

extern "C" {
#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/regist.h>
}

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace brp::session {
namespace {

void LogCallback(ChiakiLogLevel level, const char* msg, void* user) {
    (void)user;
    const char* tag = "?";
    switch (level) {
        case CHIAKI_LOG_ERROR: tag = "error"; break;
        case CHIAKI_LOG_WARNING: tag = "warn"; break;
        case CHIAKI_LOG_INFO: tag = "info"; break;
        case CHIAKI_LOG_VERBOSE: tag = "verbose"; break;
        case CHIAKI_LOG_DEBUG: tag = "debug"; break;
    }
    std::fprintf(stderr, "[regist:%s] %s\n", tag, msg);
}

}  // namespace

struct Pairing::Impl {
    ChiakiLog log{};
    ChiakiRegist regist{};
    bool initialised = false;
    bool finished_handled = false;
    int polls_after_finish = 0;
};

Pairing::~Pairing() {
    Cancel();
    if (impl_) {
        if (impl_->initialised) {
            chiaki_regist_fini(&impl_->regist);
            impl_->initialised = false;
        }
        delete impl_;
        impl_ = nullptr;
    }
}

bool Pairing::Start(const Profile& profile, uint32_t pin) {
    Poll();

    // chiaki_lib_init() performs WSAStartup and RNG/FEC setup on Windows.
    static std::once_flag init_once;
    static ChiakiErrorCode init_result = CHIAKI_ERR_SUCCESS;
    std::call_once(init_once, [] { init_result = chiaki_lib_init(); });
    if (init_result != CHIAKI_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "chiaki_lib_init failed";
        state_.store(PairState::Failed);
        return false;
    }

    if (state_.load() == PairState::Running) {
        return false;
    }
    if (profile.host.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "No console address configured";
        state_.store(PairState::Failed);
        return false;
    }
    if (profile.psn_account_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "No PSN account id (import one or sign in)";
        state_.store(PairState::Failed);
        return false;
    }

    if (!impl_) {
        impl_ = new Impl();
    } else if (impl_->initialised) {
        chiaki_regist_fini(&impl_->regist);
        impl_->initialised = false;
    }

    std::memset(&impl_->regist, 0, sizeof(impl_->regist));
    impl_->finished_handled = false;
    chiaki_log_init(&impl_->log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO,
                    &LogCallback, this);

    std::vector<uint8_t> account_id(8, 0);
    size_t account_size = account_id.size();
    if (chiaki_base64_decode(profile.psn_account_id.c_str(), profile.psn_account_id.size(),
                             account_id.data(), &account_size) != CHIAKI_ERR_SUCCESS ||
        account_size != 8) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "PSN account id is not valid base64 for 8 bytes";
        state_.store(PairState::Failed);
        return false;
    }
    ChiakiRegistInfo info{};
    info.target = profile.device_type == "PS5" ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
    info.host = _strdup(profile.host.c_str());
    info.psn_online_id = nullptr;
    std::memcpy(info.psn_account_id, account_id.data(), account_id.size());
    info.pin = pin;
    info.console_pin = pin;
    info.holepunch_info = nullptr;
    info.broadcast = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Registering with the console...";
    }
    credentials_ready_.store(false);
    state_.store(PairState::Running);
    const ChiakiErrorCode err =
        chiaki_regist_start(&impl_->regist, &impl_->log, &info, &Pairing::OnEvent, this);
    if (err != CHIAKI_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = std::string("Failed to start registration (") +
                   chiaki_error_string(err) + ")";
        state_.store(PairState::Failed);
        return false;
    }

    impl_->initialised = true;
    return true;
}

void Pairing::OnEvent(ChiakiRegistEvent* event, void* user) {
    auto* self = static_cast<Pairing*>(user);
    if (!event) {
        return;
    }

    std::lock_guard<std::mutex> lock(self->mutex_);

    if (event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host) {
        const ChiakiRegisteredHost* host = event->registered_host;
        std::memcpy(self->credentials_.rp_key.data(), host->rp_key, 16);
        std::memcpy(self->credentials_.rp_regist_key.data(), host->rp_regist_key, 16);
        self->credentials_.rp_key_type = host->rp_key_type;
        self->credentials_.console_pin = host->console_pin;
        self->credentials_ready_.store(true);
        self->message_ = "Paired successfully";
        self->state_.store(PairState::Success);
        return;
    }

    if (event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED) {
        self->message_ = "Registration cancelled";
        self->state_.store(PairState::Failed);
        return;
    }

    self->message_ = "Registration failed (wrong PIN or unreachable console)";
    self->state_.store(PairState::Failed);
}

void Pairing::Poll() {
    if (!impl_) {
        return;
    }
    if (impl_->initialised && state_.load() != PairState::Running && !impl_->finished_handled) {
        chiaki_regist_fini(&impl_->regist);
        impl_->finished_handled = true;
        impl_->initialised = false;
    }
}

void Pairing::Cancel() {
    if (impl_ && impl_->initialised) {
        chiaki_regist_stop(&impl_->regist);
        chiaki_regist_fini(&impl_->regist);
        impl_->initialised = false;
        impl_->finished_handled = true;
        state_.store(PairState::Failed);
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Registration cancelled";
    }
}

std::string Pairing::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

bool Pairing::TakeCredentials(Credentials& out) {
    if (!credentials_ready_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    out = credentials_;
    credentials_ready_.store(false);
    return true;
}

}  // namespace brp::session
