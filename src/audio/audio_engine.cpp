#include "audio/audio_engine.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace brp::audio {
namespace {

// kFriendlyName, defined locally to avoid the devpkey header's
// DEFINE_PROPERTYKEY churn.
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr int kSourceRate = 48000;
constexpr int kSourceChannels = 2;

std::string Narrow(const wchar_t* text) {
    if (!text) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    }
    return out;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsControllerName(const std::string& name) {
    const std::string lower = Lower(name);
    return lower.find("dualsense") != std::string::npos ||
           lower.find("wireless controller") != std::string::npos ||
           lower.find("dualshock") != std::string::npos;
}

}  // namespace

Engine::~Engine() {
    Close();
}

std::vector<DeviceInfo> Engine::Enumerate() {
    std::vector<DeviceInfo> result;

    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&enumerator)))) {
        std::string default_id;
        ComPtr<IMMDevice> default_device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(default_device->GetId(&id)) && id) {
                default_id = Narrow(id);
                ::CoTaskMemFree(id);
            }
        }

        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            if (SUCCEEDED(collection->GetCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<IMMDevice> device;
                    if (FAILED(collection->Item(i, &device))) {
                        continue;
                    }
                    LPWSTR id = nullptr;
                    if (FAILED(device->GetId(&id)) || !id) {
                        continue;
                    }

                    DeviceInfo info;
                    info.id = Narrow(id);
                    info.is_default = info.id == default_id;
                    ::CoTaskMemFree(id);

                    ComPtr<IPropertyStore> props;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                        PROPVARIANT value;
                        ::PropVariantInit(&value);
                        if (SUCCEEDED(props->GetValue(kFriendlyName, &value)) &&
                            value.vt == VT_LPWSTR) {
                            info.name = Narrow(value.pwszVal);
                        }
                        ::PropVariantClear(&value);
                    }
                    if (info.name.empty()) {
                        info.name = "Unnamed output";
                    }
                    info.is_controller = IsControllerName(info.name);
                    result.push_back(info);
                }
            }
        }
    }

    if (uninit) {
        ::CoUninitialize();
    }

    std::stable_sort(result.begin(), result.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        if (a.is_default != b.is_default) {
            return a.is_default;
        }
        if (a.is_controller != b.is_controller) {
            return a.is_controller;
        }
        return a.name < b.name;
    });
    return result;
}

bool Engine::Open(const std::string& device_id, int buffer_ms) {
    Close();

    device_id_ = device_id;
    buffer_ms_ = std::clamp(buffer_ms, 5, 200);

    ring_.assign(kRingFrames * kSourceChannels, 0.0f);
    write_frame_.store(0);
    read_frame_.store(0);
    dropped_frames_.store(0);
    played_frames_.store(0);
    underruns_.store(0);
    peak_.store(0.0f);
    src_pos_ = -1.0;

    ready_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    render_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_event_ || !render_event_) {
        Close();
        return false;
    }

    running_.store(true);
    opened_ok_.store(false);
    thread_ = std::thread(&Engine::ThreadMain, this);

    const DWORD wait = ::WaitForSingleObject(ready_event_, 3000);
    if (wait != WAIT_OBJECT_0 || !opened_ok_.load()) {
        running_.store(false);
        if (render_event_) {
            ::SetEvent(render_event_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        Close();
        return false;
    }
    return true;
}

void Engine::Close() {
    running_.store(false);
    if (render_event_) {
        ::SetEvent(render_event_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (ready_event_) {
        ::CloseHandle(ready_event_);
        ready_event_ = nullptr;
    }
    if (render_event_) {
        ::CloseHandle(render_event_);
        render_event_ = nullptr;
    }
    opened_ok_.store(false);
}

void Engine::ThreadMain() {
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)init;

    auto fail = [this]() {
        opened_ok_.store(false);
        ::SetEvent(ready_event_);
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)))) {
        fail();
        return;
    }

    ComPtr<IMMDevice> device;
    if (device_id_.empty()) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            fail();
            return;
        }
    } else {
        const std::wstring wide = Widen(device_id_);
        if (FAILED(enumerator->GetDevice(wide.c_str(), &device))) {
            fail();
            return;
        }
    }

    // Friendly name for the UI.
    {
        ComPtr<IPropertyStore> props;
        PROPVARIANT value;
        ::PropVariantInit(&value);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(props->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) {
            std::lock_guard<std::mutex> lock(name_mutex_);
            device_name_ = Narrow(value.pwszVal);
        }
        ::PropVariantClear(&value);
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        fail();
        return;
    }

    // Ask for 48 kHz float stereo and let Windows sample-rate-convert into the
    // endpoint. This avoids shipping or installing any audio driver.
    WAVEFORMATEXTENSIBLE wanted{};
    wanted.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wanted.Format.nChannels = kSourceChannels;
    wanted.Format.nSamplesPerSec = kSourceRate;
    wanted.Format.wBitsPerSample = 32;
    wanted.Format.nBlockAlign = kSourceChannels * 4;
    wanted.Format.nAvgBytesPerSec = kSourceRate * kSourceChannels * 4;
    wanted.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wanted.Samples.wValidBitsPerSample = 32;
    wanted.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wanted.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(buffer_ms_) * 10000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    bool autoconvert = true;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                                    &wanted.Format, nullptr);
    if (FAILED(hr)) {
        // Fall back to the endpoint's native mix format and convert manually.
        autoconvert = false;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) {
            fail();
            return;
        }
        mix_rate_ = static_cast<int>(mix->nSamplesPerSec);
        mix_channels_ = mix->nChannels;
        mix_bits_ = mix->wBitsPerSample;
        mix_float_ = false;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
            mix_float_ = ::IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            mix_float_ = true;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                duration, 0, mix, nullptr);
        ::CoTaskMemFree(mix);
        if (FAILED(hr)) {
            fail();
            return;
        }
    } else {
        mix_rate_ = kSourceRate;
        mix_channels_ = kSourceChannels;
        mix_bits_ = 32;
        mix_float_ = true;
    }

    {
        std::lock_guard<std::mutex> lock(name_mutex_);
        char text[128];
        std::snprintf(text, sizeof(text), "%d Hz · %s · %s", mix_rate_,
                      mix_channels_ == 1 ? "mono" : (mix_channels_ == 2 ? "stereo" : "multichannel"),
                      mix_float_ ? "float32" : (mix_bits_ == 16 ? "16-bit" : "pcm"));
        format_text_ = text;
        if (device_name_.empty()) {
            device_name_ = "Audio device";
        }
    }

    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames))) {
        fail();
        return;
    }
    if (FAILED(client->SetEventHandle(render_event_))) {
        fail();
        return;
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
        fail();
        return;
    }

    // Prefill silence so the first callback has something to play.
    BYTE* data = nullptr;
    if (SUCCEEDED(render->GetBuffer(buffer_frames, &data))) {
        render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (FAILED(client->Start())) {
        if (mmcss) {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
        fail();
        return;
    }

    opened_ok_.store(true);
    ::SetEvent(ready_event_);

    std::vector<float> staging;

    while (running_.load()) {
        const DWORD wait = ::WaitForSingleObject(render_event_, 200);
        if (wait != WAIT_OBJECT_0) {
            continue;
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            break;
        }
        const UINT32 available = buffer_frames - padding;
        if (available == 0) {
            continue;
        }

        if (FAILED(render->GetBuffer(available, &data))) {
            break;
        }

        const int frames = static_cast<int>(available);

        if (autoconvert && mix_float_ && mix_rate_ == kSourceRate && mix_channels_ == 2) {
            auto* out = reinterpret_cast<float*>(data);
            RenderChunk(out, frames);
        } else {
            const int needed =
                (mix_rate_ == kSourceRate)
                    ? frames
                    : static_cast<int>(std::ceil(static_cast<double>(frames) * kSourceRate /
                                                 mix_rate_) +
                                       2);
            staging.resize(static_cast<size_t>(needed) * kSourceChannels);
            RenderChunk(staging.data(), needed);
            ConvertAndWrite(data, staging.data(), needed, frames);
        }

        render->ReleaseBuffer(available, 0);
        played_frames_.fetch_add(static_cast<uint64_t>(frames));
    }

    client->Stop();
    if (mmcss) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
    ::CoUninitialize();
}

void Engine::RenderChunk(float* dst, int frames) {
    if (frames <= 0) {
        return;
    }

    const uint64_t write = write_frame_.load(std::memory_order_acquire);
    uint64_t read = read_frame_.load(std::memory_order_relaxed);

    const uint64_t available = write > read ? write - read : 0;
    const int from_ring = static_cast<int>(std::min<uint64_t>(available, static_cast<uint64_t>(frames)));

    float peak = 0.0f;
    for (int i = 0; i < from_ring; ++i) {
        const float* src = &ring_[((read + i) & (kRingFrames - 1)) * kSourceChannels];
        dst[i * 2 + 0] = src[0];
        dst[i * 2 + 1] = src[1];
        peak = std::max(peak, std::max(std::fabs(src[0]), std::fabs(src[1])));
    }

    if (from_ring < frames) {
        std::memset(dst + static_cast<size_t>(from_ring) * 2, 0,
                    static_cast<size_t>(frames - from_ring) * 2 * sizeof(float));
        // Only count an underrun when audio was recently flowing. An idle engine
        // feeding silence is not a dropout.
        const uint64_t now = ::GetTickCount64();
        if (running_.load() && last_data_ms_ != 0 && now - last_data_ms_ < 300) {
            underruns_.fetch_add(1);
        }
    } else if (from_ring > 0) {
        last_data_ms_ = ::GetTickCount64();
    }

    read += static_cast<uint64_t>(from_ring);
    read_frame_.store(read, std::memory_order_release);
    peak_.store(peak);
}

void Engine::ConvertAndWrite(BYTE* dst, const float* source, int source_frames, int out_frames) {
    for (int i = 0; i < out_frames; ++i) {
        const double pos = (mix_rate_ == kSourceRate)
                               ? static_cast<double>(i)
                               : static_cast<double>(i) * kSourceRate / mix_rate_;
        int i0 = static_cast<int>(pos);
        int i1 = std::min(i0 + 1, source_frames - 1);
        if (i0 >= source_frames) {
            i0 = source_frames - 1;
            i1 = i0;
        }
        const float frac = static_cast<float>(pos - i0);
        const float l = source[i0 * 2 + 0] * (1.0f - frac) + source[i1 * 2 + 0] * frac;
        const float r = source[i0 * 2 + 1] * (1.0f - frac) + source[i1 * 2 + 1] * frac;

        for (int c = 0; c < mix_channels_; ++c) {
            float value = (c == 0) ? l : (c == 1 ? r : 0.0f);
            if (mix_channels_ == 1) {
                value = (l + r) * 0.5f;
            }
            if (mix_float_) {
                reinterpret_cast<float*>(dst)[static_cast<size_t>(i) * mix_channels_ + c] = value;
            } else if (mix_bits_ == 16) {
                reinterpret_cast<int16_t*>(dst)[static_cast<size_t>(i) * mix_channels_ + c] =
                    static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
            } else {
                reinterpret_cast<int32_t*>(dst)[static_cast<size_t>(i) * mix_channels_ + c] =
                    static_cast<int32_t>(std::clamp(value, -1.0f, 1.0f) * 2147483647.0f);
            }
        }
    }
}

void Engine::Push(const float* interleaved, int frames) {
    if (!interleaved || frames <= 0 || ring_.empty()) {
        return;
    }

    uint64_t write = write_frame_.load(std::memory_order_relaxed);
    const uint64_t read = read_frame_.load(std::memory_order_acquire);
    const uint64_t used = write - read;
    const uint64_t free_frames = kRingFrames > used ? kRingFrames - used : 0;

    if (static_cast<uint64_t>(frames) > free_frames) {
        dropped_frames_.fetch_add(static_cast<uint64_t>(frames) - free_frames);
        frames = static_cast<int>(free_frames);
    }
    if (frames <= 0) {
        return;
    }

    for (int i = 0; i < frames; ++i) {
        float* dst = &ring_[((write + static_cast<uint64_t>(i)) & (kRingFrames - 1)) * kSourceChannels];
        dst[0] = interleaved[i * 2 + 0];
        dst[1] = interleaved[i * 2 + 1];
    }
    write_frame_.store(write + static_cast<uint64_t>(frames), std::memory_order_release);
}

void Engine::PlayTestTone(double seconds) {
    const int total = static_cast<int>(seconds * kSourceRate);
    std::vector<float> buffer(480 * 2);
    double phase = 0.0;

    for (int produced = 0; produced < total; produced += 480) {
        const int block = std::min(480, total - produced);
        for (int i = 0; i < block; ++i) {
            const int index = produced + i;
            const double t = static_cast<double>(index) / kSourceRate;
            const double fade = std::min({1.0, t / 0.02, (seconds - t) / 0.05});
            const float sample = static_cast<float>(0.18 * std::max(0.0, fade) *
                                                    std::sin(phase * 2.0 * 3.141592653589793));
            phase += 440.0 / kSourceRate;
            buffer[i * 2 + 0] = sample;
            buffer[i * 2 + 1] = sample;
        }
        Push(buffer.data(), block);
    }
}

std::string Engine::device_name() const {
    std::lock_guard<std::mutex> lock(name_mutex_);
    return device_name_;
}

std::string Engine::format_text() const {
    std::lock_guard<std::mutex> lock(name_mutex_);
    return format_text_;
}

double Engine::latency_ms() const {
    if (!running_.load()) {
        return 0.0;
    }
    const uint64_t write = write_frame_.load();
    const uint64_t read = read_frame_.load();
    const double queued = static_cast<double>(write - read) * 1000.0 / kSourceRate;
    return static_cast<double>(buffer_ms_) + queued;
}

}  // namespace brp::audio
