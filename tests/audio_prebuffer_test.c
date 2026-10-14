#include "audio_prebuffer.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    audio_prebuffer_t state = {0};
    // 150 ms of 16 kHz mono PCM. A lone tail must not wait for another packet.
    assert(audio_prebuffer_wait(&state, 100, 640, 4800, 32000));
    assert(audio_prebuffer_wait(&state, 249, 640, 4800, 32000));
    assert(!audio_prebuffer_wait(&state, 250, 640, 4800, 32000));
    // Once started, low water drains; it never rearms at two remaining frames.
    assert(!audio_prebuffer_wait(&state, 270, 640, 4800, 32000));
    assert(!audio_prebuffer_wait(&state, 290, 640, 4800, 32000));
    audio_prebuffer_reset(&state); // actual underrun or flush
    assert(!audio_prebuffer_wait(&state, 400, 4800, 4800, 32000));
    audio_prebuffer_reset(&state);
    // 400 ms DTX gaps cannot stretch a 150 ms wait across several packets.
    assert(audio_prebuffer_wait(&state, 800, 640, 4800, 32000));
    assert(!audio_prebuffer_wait(&state, 950, 640, 4800, 32000));
    audio_prebuffer_reset(&state);
    assert(audio_prebuffer_wait(&state, UINT32_MAX - 50, 640, 4800, 32000));
    assert(!audio_prebuffer_wait(&state, 99, 640, 4800, 32000));
    audio_prebuffer_reset(&state);
    assert(!audio_prebuffer_wait(&state, 0, 640, 0, 32000));
    // Actual BOX-3 output is 24 kHz mono; the policy also supports stereo.
    for (uint32_t bytes_per_second = 48000; bytes_per_second <= 192000; bytes_per_second *= 2) {
        audio_prebuffer_reset(&state);
        uint32_t target = bytes_per_second * 150 / 1000;
        assert(audio_prebuffer_wait(&state, 1000, 1, target, bytes_per_second));
        assert(!audio_prebuffer_wait(&state, 1150, 1, target, bytes_per_second));
    }
    puts("audio prebuffer deadline, tail, refill and clock-wrap checks passed");
}
