#ifndef YUYV_SCALE_H
#define YUYV_SCALE_H

#include <cstddef>
#include <cstdint>

// Shrinks a packed YUYV (Y0 U Y1 V) frame by an integer factor by picking
// every `factor`-th pixel: no filtering, enough to honour a still's max_edge.
// The output width is even, as YUYV needs. `dst` holds out_width * out_height
// * 2 bytes. Returns false for a factor below 1 or a frame too small to keep.
inline bool ScaleYuyv(const uint8_t* src, int width, int height, int factor, uint8_t* dst,
                      int& out_width, int& out_height)
{
    if (src == nullptr || dst == nullptr || factor < 1 || width < 2 || height < 1) return false;
    out_width = (width / factor) & ~1;
    out_height = height / factor;
    if (out_width < 2 || out_height < 1) return false;
    const size_t stride = static_cast<size_t>(width) * 2;
    for (int y = 0; y < out_height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * factor * stride;
        uint8_t* out = dst + static_cast<size_t>(y) * out_width * 2;
        for (int x = 0; x < out_width; x += 2) {
            const int a = x * factor;            // source pixel of the left output pixel
            const int b = (x + 1) * factor;      // and of the right one
            const uint8_t* pair = row + (a / 2) * 4;   // macropixel holding `a`: its chroma
            out[0] = row[a * 2];
            out[1] = pair[1];
            out[2] = row[b * 2];
            out[3] = pair[3];
            out += 4;
        }
    }
    return true;
}

// The smallest integer factor that brings the longer side within `max_edge`.
inline int YuyvScaleFactor(int width, int height, int max_edge)
{
    const int longer = width > height ? width : height;
    if (max_edge <= 0 || longer <= max_edge) return 1;
    return (longer + max_edge - 1) / max_edge;
}

#endif  // YUYV_SCALE_H
