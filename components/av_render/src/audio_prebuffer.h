#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A prebuffer is a bounded wait after an actual underrun, not a requirement
 * that every tail accumulate a minimum number of packets (DTX may not send
 * them). The renderer remains the sole queue and playout owner. */
typedef struct {
    bool running;
    bool waiting;
    uint32_t started_ms;
} audio_prebuffer_t;

static inline void audio_prebuffer_reset(audio_prebuffer_t *state)
{
    *state = (audio_prebuffer_t){0};
}

static inline bool audio_prebuffer_wait(audio_prebuffer_t *state, uint32_t now_ms,
                                       uint32_t queued_bytes, uint32_t threshold_bytes,
                                       uint32_t bytes_per_second)
{
    if (state->running || !threshold_bytes || !bytes_per_second) return false;
    if (!state->waiting) {
        state->waiting = true;
        state->started_ms = now_ms;
    }
    uint32_t budget_ms = (uint32_t)(((uint64_t)threshold_bytes * 1000 +
                                    bytes_per_second - 1) / bytes_per_second);
    if (queued_bytes >= threshold_bytes || now_ms - state->started_ms >= budget_ms) {
        state->running = true;
        state->waiting = false;
        return false;
    }
    return true;
}
