// Decodes the first N frames of an HEVC file and dumps RGBA + screen-space
// motion vectors (R16G16 half floats) for the DLSSNR probe.
//
// Usage: mv_dump <file> [frames] [out_dir]

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

uint16_t FloatToHalf(float value) {
    // IEEE 754 binary32 -> binary16, round-to-nearest-even is not critical here.
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<uint16_t>(sign | (mantissa >> 13));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 (mantissa >> 13));
}

void WriteHalfPair(uint8_t* pixel, float x, float y) {
    const uint16_t hx = FloatToHalf(std::clamp(x, -64.0f, 64.0f));
    const uint16_t hy = FloatToHalf(std::clamp(y, -64.0f, 64.0f));
    std::memcpy(pixel + 0, &hx, 2);
    std::memcpy(pixel + 2, &hy, 2);
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.string().c_str(), "wb") != 0 || !file) {
        return false;
    }
    std::fwrite(data, 1, size, file);
    std::fclose(file);
    return true;
}

bool SaveBmp(const std::vector<uint8_t>& rgba, int width, int height,
             const std::filesystem::path& path) {
    const uint32_t row = static_cast<uint32_t>(width) * 4;
    const uint32_t image_bytes = row * static_cast<uint32_t>(height);
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.string().c_str(), "wb") != 0 || !file) {
        return false;
    }
    unsigned char header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t file_size = 54 + image_bytes;
    std::memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const uint32_t info = 40;
    std::memcpy(header + 14, &info, 4);
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    const uint16_t planes = 1;
    std::memcpy(header + 26, &planes, 2);
    const uint16_t bpp = 32;
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::vector<unsigned char> line(row);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* in = rgba.data() + static_cast<size_t>(y) * row;
        for (int x = 0; x < width; ++x) {
            line[x * 4 + 0] = in[x * 4 + 2];
            line[x * 4 + 1] = in[x * 4 + 1];
            line[x * 4 + 2] = in[x * 4 + 0];
            line[x * 4 + 3] = 255;
        }
        std::fwrite(line.data(), 1, row, file);
    }
    std::fclose(file);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: mv_dump <file> [frames] [out_dir]\n");
        return 2;
    }
    const int wanted = argc > 2 ? std::max(1, std::atoi(argv[2])) : 6;
    const std::filesystem::path out_dir =
        argc > 3 ? argv[3] : std::filesystem::path("build/mv");
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, argv[1], nullptr, nullptr) < 0) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        std::fprintf(stderr, "cannot read stream info\n");
        return 1;
    }
    const int stream_index =
        av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::fprintf(stderr, "no video stream\n");
        return 1;
    }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "no decoder\n");
        return 1;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(context, stream->codecpar);
    context->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
    context->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    std::printf("decoder: %s\n", codec->name);
    if (avcodec_open2(context, codec, nullptr) < 0) {
        std::fprintf(stderr, "cannot open decoder\n");
        return 1;
    }

    int width = context->width;
    int height = context->height;
    SwsContext* scaler = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int dumped = 0;
    int mv_frames = 0;
    int total_vectors = 0;

    while (dumped < wanted && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(context, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);
        while (dumped < wanted && avcodec_receive_frame(context, frame) == 0) {
            width = frame->width;
            height = frame->height;
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
            scaler = sws_getCachedContext(scaler, width, height,
                                          static_cast<AVPixelFormat>(frame->format), width,
                                          height, AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr,
                                          nullptr, nullptr);
            if (!scaler) {
                std::fprintf(stderr, "sws context failed\n");
                return 1;
            }
            const int* coeffs = sws_getCoefficients(SWS_CS_ITU709);
            sws_setColorspaceDetails(scaler, coeffs, 0, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                     0, 1 << 16, 1 << 16);
            uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
            int dst_linesize[4] = {width * 4, 0, 0, 0};
            sws_scale(scaler, frame->data, frame->linesize, 0, height, dst, dst_linesize);

            std::vector<uint8_t> motion(static_cast<size_t>(width) * height * 4, 0);
            int vectors = 0;
            std::printf("  side data: %d\n", frame->nb_side_data);
            for (int s = 0; s < frame->nb_side_data; ++s) {
                std::printf("    type %d size %zu\n", static_cast<int>(frame->side_data[s]->type),
                            frame->side_data[s]->size);
            }
            if (const AVFrameSideData* side =
                    av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS)) {
                const auto* mvs = reinterpret_cast<const AVMotionVector*>(side->data);
                const size_t count = side->size / sizeof(AVMotionVector);
                for (size_t i = 0; i < count; ++i) {
                    const AVMotionVector& mv = mvs[i];
                    if (mv.source > 0 || mv.motion_scale == 0) {
                        continue;  // future reference: skip for now
                    }
                    const float dx = static_cast<float>(mv.motion_x) /
                                     static_cast<float>(mv.motion_scale);
                    const float dy = static_cast<float>(mv.motion_y) /
                                     static_cast<float>(mv.motion_scale);
                    for (int by = 0; by < mv.h; ++by) {
                        const int y = mv.dst_y + by;
                        if (y < 0 || y >= height) {
                            continue;
                        }
                        for (int bx = 0; bx < mv.w; ++bx) {
                            const int x = mv.dst_x + bx;
                            if (x < 0 || x >= width) {
                                continue;
                            }
                            uint8_t* pixel = motion.data() +
                                             (static_cast<size_t>(y) * width + x) * 4;
                            WriteHalfPair(pixel, dx, dy);
                        }
                    }
                    ++vectors;
                }
            }

            char name[32];
            std::snprintf(name, sizeof(name), "frame%02d", dumped);
            WriteFile(out_dir / (std::string(name) + ".rgba"), rgba.data(), rgba.size());
            WriteFile(out_dir / (std::string(name) + ".mv"), motion.data(), motion.size());
            if (dumped == 0) {
                SaveBmp(rgba, width, height, out_dir / "input.bmp");
            }
            std::printf("frame %d: %dx%d, %d motion vectors\n", dumped, width, height, vectors);
            if (vectors > 0) {
                ++mv_frames;
                total_vectors += vectors;
            }
            ++dumped;
            av_frame_unref(frame);
        }
    }

    std::printf("dumped %d frames (%d with motion, %d vectors) to %s\n", dumped, mv_frames,
                total_vectors, out_dir.string().c_str());
    if (scaler) {
        sws_freeContext(scaler);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return dumped > 0 ? 0 : 1;
}
