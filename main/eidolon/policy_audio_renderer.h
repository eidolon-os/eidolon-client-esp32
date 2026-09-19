#pragma once
#include <audio_render.h>
namespace eidolon {
// PCM carries no speech/cue tag. The trusted Channel must enforce content selection;
// this final sink checks that at least one audio output remains selected.
// Takes ownership of inner on success. Uses the upstream renderer public ops.
audio_render_handle_t AllocatePolicyAudioRenderer(audio_render_handle_t inner);
}
