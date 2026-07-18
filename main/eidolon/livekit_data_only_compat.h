#ifndef EIDOLON_LIVEKIT_DATA_ONLY_COMPAT_H_
#define EIDOLON_LIVEKIT_DATA_ONLY_COMPAT_H_

#include <livekit.h>

namespace eidolon {

// LiveKit ESP32 0.3.10 initializes and starts its capture pipeline even when
// both media directions are NONE. Keep the room data-only while supplying the
// valid media provider that this SDK version requires internally.
inline bool ConfigureDataOnlyMediaProvider(livekit_room_options_t& options,
                                           esp_capture_handle_t capturer,
                                           av_render_handle_t renderer)
{
    if (capturer == nullptr || renderer == nullptr) {
        return false;
    }
    options.publish.kind = LIVEKIT_MEDIA_TYPE_NONE;
    options.publish.capturer = capturer;
    options.subscribe.kind = LIVEKIT_MEDIA_TYPE_NONE;
    options.subscribe.renderer = renderer;
    return true;
}

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_DATA_ONLY_COMPAT_H_
