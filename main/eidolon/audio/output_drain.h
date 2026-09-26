#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace eidolon {
// After the presentation's final renderer write, advance the *actual* DMA ring
// by two complete capacities of silence. Successful ordered I2S writes can then
// leave only silence pending, even if the final speech write ended mid-buffer.
// This is a byte-count barrier, not an RMS/idle timeout. Caller must serialize
// with other writers and supply the current presentation's cancellation fence.
// Write must be bounded and report the exact bytes accepted; it must not claim
// bytes still held in a software queue ahead of I2S.
template<class Write, class Current>
bool DrainAudioOutput(size_t dma_bytes, Write write, Current current) {
    if (!dma_bytes || dma_bytes > 65536 || !current()) return false;
    const std::array<uint8_t, 640> silence{};
    size_t remaining = dma_bytes * 2;
    while (remaining) {
        if (!current()) return false;
        const size_t count = std::min(remaining, silence.size());
        size_t written = 0;
        if (!write(silence.data(), count, written) || written != count) return false;
        remaining -= written;
    }
    return current();
}
} // namespace eidolon
