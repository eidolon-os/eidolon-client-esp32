#include "policy_audio_renderer.h"
#include "output_policy.h"
namespace eidolon {
namespace {
audio_render_handle_t Init(void* cfg, int size) {
    return size == sizeof(audio_render_handle_t) ? *static_cast<audio_render_handle_t*>(cfg) : nullptr;
}
int Write(audio_render_handle_t inner, av_render_audio_frame_t* frame) {
    if (!CurrentOutputGate().Allows(presentation::Output::Speech)) return 0;
    return audio_render_write(inner, frame);
}
}
audio_render_handle_t AllocatePolicyAudioRenderer(audio_render_handle_t inner) {
    if (!inner) return nullptr;
    audio_render_cfg_t cfg = {};
    cfg.ops = {Init, audio_render_open, Write, audio_render_get_latency,
               audio_render_get_frame_info, audio_render_set_speed,
               audio_render_close, audio_render_free_handle};
    cfg.cfg = &inner;
    cfg.cfg_size = sizeof(inner);
    return audio_render_alloc_handle(&cfg);
}
}
