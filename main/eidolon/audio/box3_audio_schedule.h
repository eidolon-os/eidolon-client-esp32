#pragma once
#include <cstring>

namespace eidolon::audio {
// BOX-3 full-duplex budget: I2S capture/AFE stay on core 0; Opus decode
// shares core 1 with the native encoder/network tasks. Capture must precede
// priority-10 UI work, and AFE output must drain before more input is fed.
// This profile does not change codec quality, queue capacities or native stacks.
inline constexpr unsigned kBox3CapturePriority = 11;
inline constexpr unsigned kBox3AfeOutputPriority = 12;
inline constexpr unsigned kBox3DecodeCore = 1;
// The native Opus encoder is priority 10. Equal priority gives both codec
// stages time slices; a higher-priority decoder can starve uplink encoding
// for longer than the PCM queue's 500 ms budget during a receive burst.
inline constexpr unsigned kBox3CodecPriority = 10;

template<class ThreadConfig>
void ApplyBox3AudioPlacement(const char* name, ThreadConfig& config) {
    if (name && std::strcmp(name, "Adec") == 0) {
        config.core_id = kBox3DecodeCore;
        config.priority = kBox3CodecPriority;
    }
}
} // namespace eidolon::audio
