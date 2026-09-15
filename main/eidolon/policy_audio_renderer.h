#pragma once
#include <audio_render.h>
namespace eidolon {
// Takes ownership of inner on success. Uses the upstream renderer public ops.
audio_render_handle_t AllocatePolicyAudioRenderer(audio_render_handle_t inner);
}
