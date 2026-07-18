#include <cassert>

#include "eidolon/livekit_data_only_compat.h"

namespace {

void TestRequiresCompleteMediaProvider()
{
    livekit_room_options_t options = {};
    auto* capturer = reinterpret_cast<esp_capture_handle_t>(0x1);
    auto* renderer = reinterpret_cast<av_render_handle_t>(0x2);

    assert(!eidolon::ConfigureDataOnlyMediaProvider(options, nullptr, renderer));
    assert(!eidolon::ConfigureDataOnlyMediaProvider(options, capturer, nullptr));
}

void TestKeepsRoomDataOnlyWhileSupplyingSdkProvider()
{
    livekit_room_options_t options = {};
    options.publish.kind = LIVEKIT_MEDIA_TYPE_AUDIO;
    options.subscribe.kind = LIVEKIT_MEDIA_TYPE_VIDEO;
    auto* capturer = reinterpret_cast<esp_capture_handle_t>(0x1);
    auto* renderer = reinterpret_cast<av_render_handle_t>(0x2);

    assert(eidolon::ConfigureDataOnlyMediaProvider(options, capturer, renderer));
    assert(options.publish.kind == LIVEKIT_MEDIA_TYPE_NONE);
    assert(options.subscribe.kind == LIVEKIT_MEDIA_TYPE_NONE);
    assert(options.publish.capturer == capturer);
    assert(options.subscribe.renderer == renderer);
}

}  // namespace

int main()
{
    TestRequiresCompleteMediaProvider();
    TestKeepsRoomDataOnlyWhileSupplyingSdkProvider();
    return 0;
}
