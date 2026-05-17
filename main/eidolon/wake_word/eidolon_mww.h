#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t probability_cutoff;       // 0-255 maps to 0.0-1.0
    uint8_t sliding_window_size;      // default 10 for hey_jarvis
    uint32_t cooldown_ms;             // min time between detections
} eidolon_mww_config_t;

esp_err_t eidolon_mww_init(const eidolon_mww_config_t* config);
void eidolon_mww_deinit(void);
void eidolon_mww_reset(void);

/** Feed 16 kHz mono PCM; may be called with arbitrary chunk sizes. */
void eidolon_mww_feed_pcm(const int16_t* samples, size_t count);

/** Returns true once per detection until cooldown elapses. */
bool eidolon_mww_poll_detected(void);

#ifdef __cplusplus
}
#endif
