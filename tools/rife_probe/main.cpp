// RIFE probe: loads a TensorRT engine through brp's dynamic loader and times
// interpolation on synthetic frames. No app, no stream: this only proves the
// runtime path and gives a latency number for the resolution in the engine.

#include "media/rife.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

double NowMs() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string Arg(int argc, char** argv, const char* name, const char* fallback) {
    const size_t len = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, len) == 0 && argv[i][len] == '=') {
            return argv[i] + len + 1;
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string runtime = Arg(argc, argv, "--runtime", "");
    const std::string engine = Arg(argc, argv, "--engine", "");
    const int iterations = std::atoi(Arg(argc, argv, "--iters", "40").c_str());
    const float ts = static_cast<float>(std::atof(Arg(argc, argv, "--ts", "0.5").c_str()));
    const bool reverse = std::strcmp(Arg(argc, argv, "--reverse", "0").c_str(), "1") == 0;
    if (runtime.empty() || engine.empty()) {
        std::fprintf(stderr,
                     "usage: rife_probe --runtime=<dir with nvinfer_10.dll and cudart64_13.dll> "
                     "--engine=<file.engine> [--iters=40]\n");
        return 2;
    }

    brp::media::Rife rife;
    std::string detail;
    if (!rife.Load(runtime, engine, detail)) {
        std::fprintf(stderr, "load failed: %s\n", detail.c_str());
        return 1;
    }
    const int w = rife.width();
    const int h = rife.height();
    std::printf("engine %dx%d\n", w, h);

    // Frame A: a moving gradient. Frame B: the same gradient shifted, so an
    // interpolation between them should land in between the two.
    std::vector<uint8_t> a(static_cast<size_t>(w) * h * 4);
    std::vector<uint8_t> b(a.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            a[i + 0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
            a[i + 1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
            a[i + 2] = 96;
            a[i + 3] = 255;
            const int sx = std::min(w - 1, x + 32);
            b[i + 0] = static_cast<uint8_t>(sx * 255 / std::max(1, w - 1));
            b[i + 1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
            b[i + 2] = 96;
            b[i + 3] = 255;
        }
    }

    const uint8_t* first = reverse ? b.data() : a.data();
    const uint8_t* second = reverse ? a.data() : b.data();
    std::vector<uint8_t> out;
    if (!rife.Process(first, second, ts, out, detail)) {
        std::fprintf(stderr, "first process failed: %s\n", detail.c_str());
        return 1;
    }

    // Sanity: the interpolated frame must differ from both inputs by a
    // reasonable amount, and its red channel should sit between them.
    double diff_a = 0.0;
    double diff_b = 0.0;
    for (size_t i = 0; i < out.size(); i += 4) {
        diff_a += std::abs(static_cast<int>(out[i]) - static_cast<int>(a[i]));
        diff_b += std::abs(static_cast<int>(out[i]) - static_cast<int>(b[i]));
    }
    const double n = static_cast<double>(w) * h;
    std::printf("mean |out-a| %.2f   mean |out-b| %.2f\n", diff_a / n, diff_b / n);

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    double convert = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const double t0 = NowMs();
        if (!rife.Process(first, second, ts, out, detail)) {
            std::fprintf(stderr, "process failed: %s\n", detail.c_str());
            return 1;
        }
        samples.push_back(NowMs() - t0);
        convert += rife.last_convert_ms();
    }
    std::sort(samples.begin(), samples.end());
    const auto pick = [&samples](double q) {
        return samples[static_cast<size_t>(q * (samples.size() - 1))];
    };
    std::printf("total  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (convert p50 %.2f ms)\n",
                pick(0.50), pick(0.95), pick(0.99), samples.back(),
                convert / static_cast<double>(iterations));
    return 0;
}
