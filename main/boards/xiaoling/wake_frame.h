#pragma once
#include <cstddef>
#include <cstdint>

namespace xiaoling {
// The chip emits fixed six-byte wake frames. Keep partial frames across UART
// reads and resynchronize after noise; other voice commands cannot start a chat.
class WakeFrameParser {
public:
    bool Feed(uint8_t byte) {
        static constexpr uint8_t frame[] = {0xa1, 0x11, 0x22, 0x33, 0x44, 0xdd};
        if (byte != frame[position_]) {
            position_ = byte == frame[0] ? 1 : 0;
            return false;
        }
        if (++position_ != sizeof(frame)) return false;
        position_ = 0;
        return true;
    }
private:
    size_t position_ = 0;
};
}
