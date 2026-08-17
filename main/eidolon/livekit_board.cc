// LiveKit media board bring-up for Waveshare ESP32-S3-Touch-AMOLED-2.06 (shared BoxAudioCodec).

#include "livekit_board.h"

#include "audio_codec.h"
#include "audio/eidolon_mic_capture.h"
#include "audio/pcm_push_capture_source.h"
#include "eidolon_audio_input.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_enc_default.h>
#include <esp_capture_defaults.h>
#include <esp_capture_sink.h>
#include <esp_check.h>
#include <esp_codec_dev.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <av_render_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <string.h>

#define TAG "EidolonLKBoard"

#define LIVEKIT_SPEAKER_VOLUME CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME

static esp_capture_sink_handle_t s_capturer;
static eidolon::EidolonMicCapture* s_mic_capture;
static audio_render_handle_t s_audio_renderer;
static av_render_handle_t s_av_renderer;
static volatile int64_t s_last_playback_us;
static volatile uint32_t s_recent_capture_rms_ppm;
static volatile uint32_t s_recent_playback_rms_ppm;

namespace {
constexpr int kPlaybackPcmMinAvgAbs = 120;
constexpr uint32_t kRmsScalePpm = 1000000;

uint32_t pcm16_rms_ppm(const uint8_t* data, int len)
{
    if (data == nullptr || len < static_cast<int>(sizeof(int16_t))) {
        return 0;
    }
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const int sample_count = len / static_cast<int>(sizeof(int16_t));
    int64_t sum_sq = 0;
    for (int i = 0; i < sample_count; ++i) {
        const int32_t sample = samples[i];
        sum_sq += static_cast<int64_t>(sample) * sample;
    }
    const double mean_sq = static_cast<double>(sum_sq) / sample_count;
    const double rms = std::sqrt(mean_sq);
    const double ppm = rms * kRmsScalePpm / 32768.0;
    return static_cast<uint32_t>(
        std::min<double>(kRmsScalePpm, std::max<double>(0.0, ppm)));
}

struct GatedAudioSource {
    esp_capture_audio_src_if_t base;
    esp_capture_audio_src_if_t* inner;
    volatile bool enabled;
};

GatedAudioSource s_gated_audio_source = {};

GatedAudioSource* gated_from_base(esp_capture_audio_src_if_t* src)
{
    return reinterpret_cast<GatedAudioSource*>(src);
}

esp_capture_err_t gated_open(esp_capture_audio_src_if_t* src)
{
    auto* gated = gated_from_base(src);
    return gated->inner->open(gated->inner);
}

esp_capture_err_t gated_get_support_codecs(esp_capture_audio_src_if_t* src,
                                           const esp_capture_format_id_t** codecs,
                                           uint8_t* num)
{
    auto* gated = gated_from_base(src);
    return gated->inner->get_support_codecs(gated->inner, codecs, num);
}

esp_capture_err_t gated_set_fixed_caps(esp_capture_audio_src_if_t* src,
                                       const esp_capture_audio_info_t* fixed_caps)
{
    auto* gated = gated_from_base(src);
    if (!gated->inner->set_fixed_caps) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    return gated->inner->set_fixed_caps(gated->inner, fixed_caps);
}

esp_capture_err_t gated_negotiate_caps(esp_capture_audio_src_if_t* src,
                                       esp_capture_audio_info_t* in_caps,
                                       esp_capture_audio_info_t* out_caps)
{
    auto* gated = gated_from_base(src);
    return gated->inner->negotiate_caps(gated->inner, in_caps, out_caps);
}

esp_capture_err_t gated_start(esp_capture_audio_src_if_t* src)
{
    auto* gated = gated_from_base(src);
    return gated->inner->start(gated->inner);
}

esp_capture_err_t gated_read_frame(esp_capture_audio_src_if_t* src,
                                   esp_capture_stream_frame_t* frame)
{
    auto* gated = gated_from_base(src);
    esp_capture_err_t ret = gated->inner->read_frame(gated->inner, frame);
    if (ret == ESP_CAPTURE_ERR_OK && frame->data && frame->size > 0) {
        if (!gated->enabled) {
            memset(frame->data, 0, frame->size);
        }
        s_recent_capture_rms_ppm = pcm16_rms_ppm(frame->data, frame->size);
    }
    return ret;
}

esp_capture_err_t gated_stop(esp_capture_audio_src_if_t* src)
{
    auto* gated = gated_from_base(src);
    return gated->inner->stop(gated->inner);
}

esp_capture_err_t gated_close(esp_capture_audio_src_if_t* src)
{
    auto* gated = gated_from_base(src);
    return gated->inner->close(gated->inner);
}

esp_capture_audio_src_if_t* build_gated_audio_source(esp_capture_audio_src_if_t* inner)
{
    s_gated_audio_source = {};
    s_gated_audio_source.base.open = gated_open;
    s_gated_audio_source.base.get_support_codecs = gated_get_support_codecs;
    s_gated_audio_source.base.set_fixed_caps = gated_set_fixed_caps;
    s_gated_audio_source.base.negotiate_caps = gated_negotiate_caps;
    s_gated_audio_source.base.start = gated_start;
    s_gated_audio_source.base.read_frame = gated_read_frame;
    s_gated_audio_source.base.stop = gated_stop;
    s_gated_audio_source.base.close = gated_close;
    s_gated_audio_source.inner = inner;
    s_gated_audio_source.enabled = true;
    return &s_gated_audio_source.base;
}
}

static int on_audio_render_reference(uint8_t* data, int len, void*)
{
    if (data == nullptr || len < static_cast<int>(sizeof(int16_t))) {
        return 0;
    }

    const auto* samples = reinterpret_cast<const int16_t*>(data);
    int sample_count = len / static_cast<int>(sizeof(int16_t));
    int64_t abs_sum = 0;
    for (int i = 0; i < sample_count; ++i) {
        int sample = samples[i];
        abs_sum += sample < 0 ? -sample : sample;
    }
    s_recent_playback_rms_ppm = pcm16_rms_ppm(data, len);
    if ((abs_sum / sample_count) >= kPlaybackPcmMinAvgAbs) {
        s_last_playback_us = esp_timer_get_time();
    }
    return 0;
}

static esp_err_t build_capturer(AudioCodec* codec)
{
    // EidolonMicCapture owns the mic and exposes it as a push-based esp_capture
    // source, replacing esp_capture's wake-word-tuned AEC source. Its topology is
    // compile-time per board capability (AFE echo cancellation vs raw mic) — the
    // per-mode mic gating is the wrapper below, not the topology.
    if (s_mic_capture == nullptr) {
        s_mic_capture = new eidolon::EidolonMicCapture();
    }
    esp_err_t err = s_mic_capture->Start(codec);
    if (err != ESP_OK) {
        return err;
    }

    esp_capture_audio_src_if_t* gated_source =
        build_gated_audio_source(s_mic_capture->CaptureSource());
    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = gated_source,
    };
    return esp_capture_open(&cfg, &s_capturer);
}

static esp_err_t build_renderer(esp_codec_dev_handle_t play_handle, uint32_t output_sample_rate)
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = play_handle,
        .cb = on_audio_render_reference,
        .fixed_clock = false,
        .ctx = nullptr,
    };
    s_audio_renderer = av_render_alloc_i2s_render(&i2s_cfg);
    if (!s_audio_renderer) {
        return ESP_FAIL;
    }

    av_render_cfg_t render_cfg = {
        .audio_render = s_audio_renderer,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    s_av_renderer = av_render_open(&render_cfg);
    if (!s_av_renderer) {
        return ESP_FAIL;
    }

    av_render_audio_frame_info_t frame_info = {};
    frame_info.sample_rate = output_sample_rate;
    frame_info.channel = 1;
    frame_info.bits_per_sample = 16;
    av_render_set_fixed_frame_info(s_av_renderer, &frame_info);
    return ESP_OK;
}

extern "C" esp_err_t eidolon_livekit_board_init(void)
{
    if (s_capturer != nullptr) {
        return ESP_OK;
    }

    auto& audio_in = eidolon::EidolonAudioInput::Instance();
    if (!audio_in.IsReady()) {
        ESP_RETURN_ON_ERROR(audio_in.Init(), TAG, "audio platform");
    }

    esp_codec_dev_handle_t play = audio_in.PlaybackHandle();
    auto* codec = audio_in.Codec();
    if (!play || !codec) {
        ESP_LOGE(TAG, "Codec playback handle / codec not available");
        return ESP_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::mutex> lock(audio_in.Mutex());
        // Output is driven by av_render through the raw play handle, so keep the
        // codec's own output path closed. Input stays managed by the codec
        // because EidolonMicCapture reads mic (+reference) PCM through it.
        if (codec->output_enabled()) {
            codec->EnableOutput(false);
        }
        codec->SetOutputVolume(LIVEKIT_SPEAKER_VOLUME);
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    ESP_RETURN_ON_ERROR(build_capturer(codec), TAG, "capturer");
    uint32_t output_sample_rate = codec && codec->output_sample_rate() > 0
                                      ? static_cast<uint32_t>(codec->output_sample_rate())
                                      : 16000;
    s_last_playback_us = 0;
    s_recent_capture_rms_ppm = 0;
    s_recent_playback_rms_ppm = 0;
    ESP_RETURN_ON_ERROR(build_renderer(play, output_sample_rate), TAG, "renderer");

    if (codec) {
        ESP_LOGI(TAG, "LiveKit board media ready (input=%d Hz, output=%d Hz)",
                 codec->input_sample_rate(), codec->output_sample_rate());
    } else {
        ESP_LOGI(TAG, "LiveKit board media ready");
    }
    return ESP_OK;
}

extern "C" esp_capture_handle_t eidolon_livekit_board_get_capturer(void)
{
    return s_capturer;
}

extern "C" av_render_handle_t eidolon_livekit_board_get_renderer(void)
{
    return s_av_renderer;
}

extern "C" int64_t eidolon_livekit_board_last_playback_us(void)
{
    return s_last_playback_us;
}

extern "C" uint32_t eidolon_livekit_board_recent_capture_rms_ppm(void)
{
    return s_recent_capture_rms_ppm;
}

extern "C" uint32_t eidolon_livekit_board_recent_playback_rms_ppm(void)
{
    return s_recent_playback_rms_ppm;
}

extern "C" esp_err_t eidolon_livekit_board_set_capture_enabled(bool enabled)
{
    if (!s_capturer) {
        return ESP_ERR_INVALID_STATE;
    }
    bool previous = s_gated_audio_source.enabled;
    s_gated_audio_source.enabled = enabled;
    if (!enabled) {
        s_recent_capture_rms_ppm = 0;
    }
    if (previous != enabled) {
        ESP_LOGI(TAG, "LiveKit capture gate %s", enabled ? "open" : "muted");
    }
    return ESP_OK;
}

extern "C" esp_err_t eidolon_livekit_board_flush_playback(void)
{
    if (!s_av_renderer) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = av_render_flush(s_av_renderer);
    if (ret != ESP_MEDIA_ERR_OK) {
        ESP_LOGW(TAG, "Failed to flush LiveKit playback: %d", ret);
        return ESP_FAIL;
    }
    s_last_playback_us = 0;
    s_recent_playback_rms_ppm = 0;
    ESP_LOGI(TAG, "LiveKit playback flushed");
    return ESP_OK;
}

extern "C" void eidolon_livekit_board_deinit(void)
{
    if (s_av_renderer) {
        av_render_close(s_av_renderer);
        s_av_renderer = nullptr;
    }
    s_audio_renderer = nullptr;

    // Quiesce the capture PRODUCER before destroying the pipeline. esp_capture_close
    // frees its internal fetch data queue with only a bounded (1 s) wait for its
    // fetch thread to exit; if a producer keeps the source hot, that fetch thread is
    // still reading when data_q_deinit frees the queue -> writes into freed memory
    // -> heap corruption (use-after-free). So stop whoever feeds the active source
    // FIRST, then close. The producer is the AFE, feeding s_mic_capture's internal
    // pcm source. The AFE instance is intentionally kept alive across sessions
    // (its long-lived tasks cannot be safely torn down); build_capturer re-arms
    // it on the next session.
    if (s_mic_capture) {
        s_mic_capture->Stop();  // stops the AFE and quiesces its pcm source
    }
    if (s_capturer) {
        esp_capture_close(s_capturer);
        s_capturer = nullptr;
    }
    s_gated_audio_source = {};
    s_last_playback_us = 0;
    s_recent_capture_rms_ppm = 0;
    s_recent_playback_rms_ppm = 0;
}
