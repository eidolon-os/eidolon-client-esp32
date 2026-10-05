#include "../main/boards/xiaoling/wake_frame.h"
#include <cassert>
#include <initializer_list>

int main() {
    xiaoling::WakeFrameParser parser;
    auto feed = [&](std::initializer_list<uint8_t> bytes) {
        int wakes = 0;
        for (auto b : bytes) wakes += parser.Feed(b);
        return wakes;
    };
    assert(feed({0xa1, 0x11, 0x22}) == 0);
    assert(feed({0x33, 0x44, 0xdd}) == 1); // fragmented UART reads
    assert(feed({0xa1, 0x11, 0x22, 0x33, 0x44, 0xcc}) == 0);
    assert(feed({0, 0xa1, 0xa1, 0x11, 0x22, 0x33, 0x44, 0xdd}) == 1);
    assert(feed({0xa1, 0x11, 0x22, 0x33, 0x44, 0xdd,
                 0xa1, 0x11, 0x22, 0x33, 0x44, 0xdd}) == 2);
    for (int b = 0; b < 256; ++b) {
        xiaoling::WakeFrameParser candidate;
        const uint8_t frame[] = {0xa1, 0x11, 0x22, 0x33, 0x44, 0xdd};
        for (int i = 0; i < 5; ++i) assert(!candidate.Feed(frame[i]));
        assert(candidate.Feed(static_cast<uint8_t>(b)) == (b == 0xdd));
    }
}
