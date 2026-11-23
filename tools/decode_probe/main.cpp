// Decodes a file (or raw Annex-B stream) through libavcodec and reports timing.
#include "media/decoder.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: decode_probe <file>\n");
        return 2;
    }

    brp::media::Decoder decoder;
    std::string error;
    if (!decoder.Open(argv[1], error)) {
        std::printf("open failed: %s\n", error.c_str());
        return 1;
    }
    if (const char* chroma = std::getenv("BRP_DECODE_CHROMA")) {
        decoder.SetChromaQuality(std::atoi(chroma));
    }

    std::printf("codec     %s\nresolution %dx%d\n", decoder.codec_name().c_str(), decoder.width(),
                decoder.height());

    brp::media::DecodedFrame frame;
    int frames = 0;
    unsigned long long hash = 1469598103934665603ull;
    const auto start = std::chrono::steady_clock::now();
    while (decoder.Next(frame, error)) {
        ++frames;
        // FNV-1a over every decoded byte, so a slice split that drops or
        // duplicates rows shows up even when the timing looks right.
        for (unsigned char byte : frame.rgba) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();

    std::printf("frames    %d\n", frames);
    std::printf("hash      %016llx\n", hash);
    if (frames > 0) {
        std::printf("total     %.1f ms   (%.2f ms/frame, %.0f fps)\n", ms, ms / frames,
                    1000.0 * frames / ms);
        std::printf("last      %dx%d rgba\n", frame.width, frame.height);
    }
    return frames > 0 ? 0 : 3;
}
