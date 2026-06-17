#ifndef EIDOLON_LIVEKIT_BOARD_H_
#define EIDOLON_LIVEKIT_BOARD_H_

#include <esp_err.h>
#include <esp_codec_dev.h>
#include <esp_capture.h>
#include <av_render.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t eidolon_livekit_board_init(void);
esp_capture_handle_t eidolon_livekit_board_get_capturer(void);
av_render_handle_t eidolon_livekit_board_get_renderer(void);
int64_t eidolon_livekit_board_last_playback_us(void);
// Drain `num_samples` of the most-recent playback PCM (the signal sent to the
// DAC) for use as the AEC software reference. Fills `out` (16 kHz mono int16);
// pads with silence on underrun (playback idle). Returns samples actually
// available. SPSC: producer is the i2s render callback, consumer is the AFE
// capture read loop.
int eidolon_livekit_board_pull_playback_reference(int16_t* out, int num_samples);
// True when the AFE detected near-end (real user) speech on the AEC-cleaned mic
// within a short hangover window. Used to distinguish genuine barge-in from echo
// during agent playback.
bool eidolon_livekit_board_near_end_active(void);
esp_err_t eidolon_livekit_board_set_capture_enabled(bool enabled);
esp_err_t eidolon_livekit_board_flush_playback(void);
void eidolon_livekit_board_deinit(void);

#ifdef __cplusplus
}
#endif

#endif  // EIDOLON_LIVEKIT_BOARD_H_
