#include "guard/guard_motion.h"

namespace eidolon {
namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

constexpr uint32_t kPixFmtYuyv = Fourcc('Y', 'U', 'Y', 'V');
constexpr uint32_t kPixFmtYuv422p = Fourcc('4', '2', '2', 'P');
constexpr uint32_t kPixFmtRgb565 = Fourcc('R', 'G', 'B', 'P');
constexpr uint32_t kPixFmtRgb565x = Fourcc('R', 'G', 'B', 'R');
constexpr uint32_t kPixFmtRgb24 = Fourcc('R', 'G', 'B', '3');
constexpr uint32_t kPixFmtGrey = Fourcc('G', 'R', 'E', 'Y');

enum class PixelFormat {
    Yuv422Planar,
    Yuyv,
    Rgb565LittleEndian,
    Rgb565BigEndian,
    Rgb24,
    Grey,
};

bool ResolvePixelFormat(uint32_t fourcc, size_t& bytes_per_pixel, PixelFormat& format)
{
    switch (fourcc) {
    case kPixFmtYuv422p:
        bytes_per_pixel = 1;
        format = PixelFormat::Yuv422Planar;
        return true;
    case kPixFmtYuyv:
        bytes_per_pixel = 2;
        format = PixelFormat::Yuyv;
        return true;
    case kPixFmtRgb565:
        bytes_per_pixel = 2;
        format = PixelFormat::Rgb565LittleEndian;
        return true;
    case kPixFmtRgb565x:
        bytes_per_pixel = 2;
        format = PixelFormat::Rgb565BigEndian;
        return true;
    case kPixFmtRgb24:
        bytes_per_pixel = 3;
        format = PixelFormat::Rgb24;
        return true;
    case kPixFmtGrey:
        bytes_per_pixel = 1;
        format = PixelFormat::Grey;
        return true;
    default:
        return false;
    }
}

}  // namespace

bool ReadGuardLuminanceImage(const CameraFrame& frame, uint8_t* output,
                             size_t output_width, size_t output_height,
                             bool center_crop_square)
{
    if (frame.data == nullptr || frame.width == 0 || frame.height == 0 ||
        frame.len == 0 || output == nullptr || output_width == 0 ||
        output_height == 0) {
        return false;
    }

    size_t bytes_per_pixel = 0;
    PixelFormat format = PixelFormat::Grey;
    if (!ResolvePixelFormat(frame.pixel_format, bytes_per_pixel, format)) {
        return false;
    }

    const size_t planar_y_bytes = static_cast<size_t>(frame.width) * frame.height;
    if (format == PixelFormat::Yuv422Planar && frame.len < planar_y_bytes * 2) {
        return false;
    }
    const size_t stride = format == PixelFormat::Yuv422Planar ? frame.width : frame.len / frame.height;
    if (stride < static_cast<size_t>(frame.width) * bytes_per_pixel) {
        return false;
    }

    size_t crop_x = 0;
    size_t crop_y = 0;
    size_t crop_width = frame.width;
    size_t crop_height = frame.height;
    if (center_crop_square) {
        const size_t side = frame.width < frame.height ? frame.width : frame.height;
        crop_x = (frame.width - side) / 2;
        crop_y = (frame.height - side) / 2;
        crop_width = side;
        crop_height = side;
    }

    for (size_t gy = 0; gy < output_height; ++gy) {
        const size_t y = crop_y +
            (gy * crop_height + crop_height / 2) / output_height;
        const size_t clamped_y = y < frame.height ? y : frame.height - 1;
        for (size_t gx = 0; gx < output_width; ++gx) {
            const size_t x = crop_x +
                (gx * crop_width + crop_width / 2) / output_width;
            const size_t clamped_x = x < frame.width ? x : frame.width - 1;
            uint8_t luma = 0;
            switch (format) {
            case PixelFormat::Yuv422Planar:
                luma = frame.data[clamped_y * stride + clamped_x];
                break;
            case PixelFormat::Yuyv:
            case PixelFormat::Grey:
            {
                const uint8_t* pixel = frame.data + clamped_y * stride + clamped_x * bytes_per_pixel;
                luma = pixel[0];
                break;
            }
            case PixelFormat::Rgb565LittleEndian:
            case PixelFormat::Rgb565BigEndian: {
                const uint8_t* pixel = frame.data + clamped_y * stride + clamped_x * bytes_per_pixel;
                const uint16_t rgb = format == PixelFormat::Rgb565BigEndian
                                         ? (static_cast<uint16_t>(pixel[0]) << 8) |
                                               static_cast<uint16_t>(pixel[1])
                                         : static_cast<uint16_t>(pixel[0]) |
                                               (static_cast<uint16_t>(pixel[1]) << 8);
                const uint8_t red = static_cast<uint8_t>(((rgb >> 11) & 0x1f) * 255 / 31);
                const uint8_t green = static_cast<uint8_t>(((rgb >> 5) & 0x3f) * 255 / 63);
                const uint8_t blue = static_cast<uint8_t>((rgb & 0x1f) * 255 / 31);
                luma = static_cast<uint8_t>((77 * red + 150 * green + 29 * blue) >> 8);
                break;
            }
            case PixelFormat::Rgb24: {
                const uint8_t* pixel = frame.data + clamped_y * stride + clamped_x * bytes_per_pixel;
                luma = static_cast<uint8_t>((77 * pixel[0] + 150 * pixel[1] + 29 * pixel[2]) >> 8);
                break;
            }
            }
            output[gy * output_width + gx] = luma;
        }
    }
    return true;
}

bool ReadGuardLuminanceGrid(const CameraFrame& frame, GuardLuminanceGrid& output)
{
    return ReadGuardLuminanceImage(frame, output.data(), kGuardMotionGridWidth,
                                   kGuardMotionGridHeight, false);
}

uint32_t GuardMotionScore(const GuardLuminanceGrid& before, const GuardLuminanceGrid& after)
{
    uint32_t total = 0;
    for (size_t i = 0; i < kGuardMotionGridPixels; ++i) {
        const int delta = static_cast<int>(after[i]) - static_cast<int>(before[i]);
        total += static_cast<uint32_t>(delta < 0 ? -delta : delta);
    }
    return total / kGuardMotionGridPixels;
}

void GuardFourcc(uint32_t value, char output[5])
{
    output[0] = static_cast<char>(value & 0xff);
    output[1] = static_cast<char>((value >> 8) & 0xff);
    output[2] = static_cast<char>((value >> 16) & 0xff);
    output[3] = static_cast<char>((value >> 24) & 0xff);
    output[4] = '\0';
}

}  // namespace eidolon
