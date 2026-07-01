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
uint32_t eidolon_livekit_board_recent_capture_rms_ppm(void);
uint32_t eidolon_livekit_board_recent_playback_rms_ppm(void);
esp_err_t eidolon_livekit_board_set_capture_enabled(bool enabled);
esp_err_t eidolon_livekit_board_flush_playback(void);
void eidolon_livekit_board_deinit(void);

#ifdef __cplusplus
}
#endif

#endif  // EIDOLON_LIVEKIT_BOARD_H_
