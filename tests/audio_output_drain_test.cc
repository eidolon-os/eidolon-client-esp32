#include "eidolon/audio/output_drain.h"
#include <cassert>
#include <deque>

int main() {
    // Adversarial partial final speech write: any initial fill up to capacity.
    for (size_t capacity : {size_t(480), size_t(1440), size_t(5760)}) {
        for (size_t fill : {size_t(1), capacity / 2, capacity}) {
            std::deque<uint8_t> ring(fill, 1);
            size_t speech_played = 0;
            const bool ok = eidolon::DrainAudioOutput(capacity,
                [&](const uint8_t* data, size_t n, size_t& written) {
                    for (size_t i = 0; i < n; ++i) {
                        assert(data[i] == 0);
                        if (ring.size() == capacity) {
                            speech_played += ring.front();
                            ring.pop_front();
                        }
                        ring.push_back(data[i]);
                    }
                    written = n;
                    return true;
                }, [] { return true; });
            assert(ok && speech_played == fill);
            for (auto b : ring) assert(b == 0);
        }
    }
    int writes = 0;
    auto write = [&](const uint8_t*, size_t n, size_t& written) {
        ++writes; written = n; return true;
    };
    assert(!eidolon::DrainAudioOutput(0, write, [] { return true; }));
    assert(!eidolon::DrainAudioOutput(65537, write, [] { return true; }));
    assert(writes == 0);
    assert(!eidolon::DrainAudioOutput(1440, write, [&] { return writes < 1; }));
    assert(writes == 1); // stop can revoke between writes
    assert(!eidolon::DrainAudioOutput(480,
        [](const uint8_t*, size_t n, size_t& written) { written = n - 1; return true; },
        [] { return true; }));
    assert(!eidolon::DrainAudioOutput(480,
        [](const uint8_t*, size_t, size_t&) { return false; }, [] { return true; }));
}
