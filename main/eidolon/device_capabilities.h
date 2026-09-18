#pragma once
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
#include <cstdint>
#include "eidolon/expression/generated/output_catalog.h"

namespace eidolon {
// Installed execution facilities, independent of Owner grants and UI layout.
struct DeviceCapabilities {
    bool policy_required = false;
    bool microphone = true;
    bool speaker = true;
    bool camera = false;
    bool dialogue_text = false;
    bool expression = false;
    bool audio_cue = false;
    uint32_t OutputMask() const {
        using presentation::Output;
        return (speaker ? static_cast<uint32_t>(Output::Speech) : 0u) |
               (dialogue_text ? static_cast<uint32_t>(Output::DialogueText) : 0u) |
               (expression ? static_cast<uint32_t>(Output::Expression) : 0u) |
               (speaker && audio_cue ? static_cast<uint32_t>(Output::AudioCue) : 0u);
    }
};
inline DeviceCapabilities CompiledDeviceCapabilities() {
    DeviceCapabilities capabilities;
#if CONFIG_EIDOLON_OUTPUT_POLICY_V1
    capabilities.policy_required = true;
#endif
#ifdef ESP_PLATFORM
#if !CONFIG_EIDOLON_CAP_MICROPHONE
    capabilities.microphone = false;
#endif
#if !CONFIG_EIDOLON_CAP_SPEAKER
    capabilities.speaker = false;
#endif
#endif
#if CONFIG_EIDOLON_CAP_DIALOGUE_TEXT
    capabilities.dialogue_text = true;
#endif
#if CONFIG_EIDOLON_CAP_EXPRESSION
    capabilities.expression = true;
#endif
#if CONFIG_EIDOLON_CAP_AUDIO_CUE
    capabilities.audio_cue = true;
#endif
    return capabilities;
}
}  // namespace eidolon
