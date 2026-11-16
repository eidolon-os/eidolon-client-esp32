// A still's max_edge is honoured by shrinking the camera's YUYV frame.
#include "boards/common/yuyv_scale.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

// A frame whose every byte says where it came from: Y = column, U = row,
// V = 255 - row, so the picked pixels can be checked exactly.
std::vector<uint8_t> Frame(int width, int height)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &frame[(static_cast<size_t>(y) * width + x) * 2];
            p[0] = static_cast<uint8_t>(x);
            p[1] = static_cast<uint8_t>(x % 2 == 0 ? y : 255 - y);   // U on even, V on odd
        }
    }
    return frame;
}

}  // namespace

int main()
{
    assert(YuyvScaleFactor(640, 480, 768) == 1);
    assert(YuyvScaleFactor(640, 480, 640) == 1);
    assert(YuyvScaleFactor(640, 480, 639) == 2);
    assert(YuyvScaleFactor(640, 480, 320) == 2);
    assert(YuyvScaleFactor(640, 480, 256) == 3);   // 213 x 160 fits under 256

    for (int factor : {2, 3}) {
        const int width = 64, height = 48;
        const auto frame = Frame(width, height);
        std::vector<uint8_t> out(static_cast<size_t>(width) * height * 2);
        int out_width = 0, out_height = 0;
        assert(ScaleYuyv(frame.data(), width, height, factor, out.data(), out_width, out_height));
        assert(out_width % 2 == 0 && out_width <= width / factor && out_height == height / factor);
        for (int y = 0; y < out_height; ++y) {
            for (int x = 0; x < out_width; x += 2) {
                const uint8_t* p = &out[(static_cast<size_t>(y) * out_width + x) * 2];
                const int source_row = y * factor;
                assert(p[0] == x * factor);              // Y of the left source pixel
                assert(p[2] == (x + 1) * factor);        // Y of the right one
                assert(p[1] == source_row);              // U of the left pixel's macropixel
                assert(p[3] == 255 - source_row);        // V of it
            }
        }
    }
    int w = 0, h = 0;
    uint8_t byte = 0;
    assert(!ScaleYuyv(&byte, 4, 4, 0, &byte, w, h));
    assert(!ScaleYuyv(&byte, 4, 1, 4, &byte, w, h));
    std::puts("yuyv scale tests passed");
    return 0;
}
