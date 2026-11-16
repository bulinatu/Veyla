// Micro-benchmark for the YUV 4:2:0 -> RGBA conversion used by the decoder.
// It isolates sws_scale so the chroma-quality flags can be compared without a
// live console. Usage: scale_probe [iterations]

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

struct Case {
    const char* name;
    int flags;
};

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 200;

    // NV12: full-size Y plane, half-size interleaved UV plane.
    std::vector<uint8_t> y(static_cast<size_t>(kWidth) * kHeight);
    std::vector<uint8_t> uv(static_cast<size_t>(kWidth) * (kHeight / 2));
    for (int row = 0; row < kHeight; ++row) {
        for (int x = 0; x < kWidth; ++x) {
            y[static_cast<size_t>(row) * kWidth + x] =
                static_cast<uint8_t>((x * 255 / (kWidth - 1) + row * 3) & 0xFF);
        }
    }
    for (size_t i = 0; i < uv.size(); i += 2) {
        uv[i + 0] = static_cast<uint8_t>((i / 2) % 256);
        uv[i + 1] = static_cast<uint8_t>((i / 3) % 256);
    }

    const uint8_t* src_data[4] = {y.data(), uv.data(), nullptr, nullptr};
    const int src_linesize[4] = {kWidth, kWidth, 0, 0};

    std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
    uint8_t* dst_data[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {kWidth * 4, 0, 0, 0};

    const Case cases[] = {
        {"bilinear", SWS_BILINEAR},
        {"bilinear + accurate", SWS_BILINEAR | SWS_ACCURATE_RND},
        {"bilinear + full + accurate", SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND},
        {"bicubic", SWS_BICUBIC},
        {"bicubic + accurate", SWS_BICUBIC | SWS_ACCURATE_RND},
        {"bicubic + full", SWS_BICUBIC | SWS_FULL_CHR_H_INT},
        {"bicubic + full + accurate", SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND},
        {"lanczos + accurate", SWS_LANCZOS | SWS_ACCURATE_RND},
        {"lanczos + full + accurate", SWS_LANCZOS | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND},
    };

    std::printf("%dx%d NV12 -> RGBA, %d iterations\n", kWidth, kHeight, iterations);
    for (const Case& test : cases) {
        SwsContext* context = sws_getContext(
            kWidth, kHeight, AV_PIX_FMT_NV12, kWidth, kHeight, AV_PIX_FMT_RGBA, test.flags,
            nullptr, nullptr, nullptr);
        if (!context) {
            std::printf("%-30s context failed\n", test.name);
            continue;
        }
        const int* coeffs = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(context, coeffs, 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                 1 << 16, 1 << 16);

        // Warm up.
        for (int i = 0; i < 5; ++i) {
            sws_scale(context, src_data, src_linesize, 0, kHeight, dst_data, dst_linesize);
        }

        std::vector<double> samples;
        samples.reserve(iterations);
        const double total_start = NowMs();
        for (int i = 0; i < iterations; ++i) {
            const double start = NowMs();
            sws_scale(context, src_data, src_linesize, 0, kHeight, dst_data, dst_linesize);
            samples.push_back(NowMs() - start);
        }
        const double total = NowMs() - total_start;
        std::sort(samples.begin(), samples.end());
        const double p50 = samples[samples.size() / 2];
        const double p95 = samples[static_cast<size_t>(samples.size() * 0.95)];
        std::printf("%-30s avg %6.2f ms  p50 %6.2f  p95 %6.2f  (%.1f fps)\n", test.name,
                    total / iterations, p50, p95, 1000.0 / (total / iterations));
        sws_freeContext(context);
    }
    return 0;
}
