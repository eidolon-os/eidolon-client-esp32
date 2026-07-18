#ifndef TEST_FAKE_LIVEKIT_H_
#define TEST_FAKE_LIVEKIT_H_

using esp_capture_handle_t = void*;
using av_render_handle_t = void*;

enum livekit_media_kind_t {
    LIVEKIT_MEDIA_TYPE_NONE = 0,
    LIVEKIT_MEDIA_TYPE_AUDIO = 1,
    LIVEKIT_MEDIA_TYPE_VIDEO = 2,
};

struct livekit_pub_options_t {
    livekit_media_kind_t kind = LIVEKIT_MEDIA_TYPE_NONE;
    esp_capture_handle_t capturer = nullptr;
};

struct livekit_sub_options_t {
    livekit_media_kind_t kind = LIVEKIT_MEDIA_TYPE_NONE;
    av_render_handle_t renderer = nullptr;
};

struct livekit_room_options_t {
    livekit_pub_options_t publish;
    livekit_sub_options_t subscribe;
};

#endif  // TEST_FAKE_LIVEKIT_H_
