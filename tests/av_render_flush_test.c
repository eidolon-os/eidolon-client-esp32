// Exercise the vendored flush implementation against a deterministic worker runtime.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum { AV_RENDER_MSG_FLUSH = 1, FLUSH_SHIFT_BITS = 8 };
typedef struct { int type; } av_render_msg_t;
typedef struct { int unused; } av_render_audio_data_t;
typedef struct { int unused; } av_render_video_data_t;
typedef struct { int unused; } av_render_audio_frame_t;
typedef struct { int unused; } av_render_video_frame_t;
typedef struct { void *thread, *data_q; bool flushing; int wait_bits; } thread_res_t;
typedef struct { thread_res_t thread_res; } decoder_t;
typedef struct { thread_res_t thread_res; bool audio_rendered, video_rendered; int sent_frame_num; } output_t;
typedef struct { decoder_t *adec_res, *vdec_res; output_t *a_render_res, *v_render_res; void *event_group; } av_render_t;
static int pending, sent, waited;
static void send_msg_to_thread(thread_res_t *thread, size_t size, av_render_msg_t *msg) {
    (void)size;
    assert(thread->thread && thread->flushing && msg->type == AV_RENDER_MSG_FLUSH);
    pending |= thread->wait_bits << FLUSH_SHIFT_BITS;
    sent++;
}
static void render_consume_all(thread_res_t *thread) { assert(thread->flushing); }
static void wait_for_events(void *group, int bits) {
    (void)group;
    assert(bits != 0); // Same precondition as FreeRTOS xEventGroupWaitBits.
    assert((pending & bits) == bits);
    pending &= ~bits;
    waited++;
}
#define _WAIT_BITS(group, bits) wait_for_events(group, bits)
#include "render_flush.inc"
int main(void) {
    // No streams, audio only, video only, both; with and without decoder workers.
    for (int mask = 0; mask < 16; ++mask) {
        decoder_t audio_dec = {.thread_res = {.thread=(void*)1, .wait_bits=1}};
        decoder_t video_dec = {.thread_res = {.thread=(void*)1, .wait_bits=2}};
        output_t audio = {.thread_res = {.thread=(void*)1, .data_q=(void*)1, .wait_bits=4}, .audio_rendered=true};
        output_t video = {.thread_res = {.thread=(void*)1, .data_q=(void*)1, .wait_bits=8}, .video_rendered=true, .sent_frame_num=7};
        av_render_t renderer = {.adec_res=(mask & 1) ? &audio_dec : NULL,
            .vdec_res=(mask & 2) ? &video_dec : NULL,
            .a_render_res=(mask & 4) ? &audio : NULL,
            .v_render_res=(mask & 8) ? &video : NULL};
        pending = sent = waited = 0;
        assert(render_flush(&renderer) == 0);
        assert(pending == 0); // Every requested worker, including audio+video, acknowledged.
        assert((mask != 0) == (sent != 0));
        if (renderer.a_render_res) assert(!audio.audio_rendered);
        if (renderer.v_render_res) assert(!video.video_rendered && video.sent_frame_num == 0);
        assert(render_flush(&renderer) == 0); // Repeated cleanup is safe.
        assert(pending == 0);
    }
    puts("av_render flush: 16 worker configurations passed");
}
