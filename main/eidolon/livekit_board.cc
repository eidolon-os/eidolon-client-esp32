// LiveKit media board bring-up for Waveshare ESP32-S3-Touch-AMOLED-2.06 (shared BoxAudioCodec).

#include "livekit_board.h"

#include "audio_codec.h"
#include "audio/eidolon_afe_capture.h"
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
#include <string.h>

#define TAG "EidolonLKBoard"

#define LIVEKIT_SPEAKER_VOLUME CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME

static esp_capture_sink_handle_t s_capturer;
static eidolon::EidolonAfeCapture* s_afe_capture;
static audio_render_handle_t s_audio_renderer;
static av_render_handle_t s_av_renderer;
static volatile int64_t s_last_playback_us;

// Software AEC reference ring. Holds the PCM sent to the DAC (captured at the
// i2s render boundary) so the AFE AEC has a clean reference — this board's
// hardware reference channel (ES7210 MIC3) is crosstalk-contaminated and unusable.
// SPSC: produced by on_audio_render_reference (render thread), consumed by the
// AFE capture read loop via eidolon_livekit_board_pull_playback_reference.
// Monotonic indices, masked on access; pow2 size for cheap masking.
static constexpr size_t kRefRingSamples = 8192;  // pow2, ~0.5 s @ 16 kHz
static int16_t s_ref_ring[kRefRingSamples];
static volatile size_t s_ref_write;  // monotonic write index (render thread)
// Playback→mic echo latency (av_render FIFO + DAC + acoustic), measured on-device
// by cross-correlating the played PCM against the mic echo (~68 ms). The reference
// is fed this far behind the write head so it aligns with the echo and the AFE
// AEC's short adaptive filter only covers the small residual. Tune if the board /
// render buffering changes.
static constexpr size_t kRefDelaySamples = 1088;  // ~68 ms @ 16 kHz

namespace {
constexpr int kPlaybackPcmMinAvgAbs = 120;

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
    if (ret == ESP_CAPTURE_ERR_OK && !gated->enabled && frame->data && frame->size > 0) {
        memset(frame->data, 0, frame->size);
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
    if ((abs_sum / sample_count) >= kPlaybackPcmMinAvgAbs) {
        s_last_playback_us = esp_timer_get_time();
    }
    // Push the played PCM into the software AEC reference ring (single producer).
    size_t w = s_ref_write;
    for (int i = 0; i < sample_count; ++i) {
        s_ref_ring[w & (kRefRingSamples - 1)] = samples[i];
        ++w;
    }
    s_ref_write = w;
    return 0;
}

static esp_err_t build_capturer(AudioCodec* codec)
{
    // Device-side AEC runs in EidolonAfeCapture (xiaozhi AfeAudioProcessor, voice
    // profile). The cancelled PCM is exposed as a push-based esp_capture source,
    // replacing esp_capture's wake-word-tuned AEC source.
    if (s_afe_capture == nullptr) {
        s_afe_capture = new eidolon::EidolonAfeCapture();
    }
    esp_err_t err = s_afe_capture->Start(codec);
    if (err != ESP_OK) {
        return err;
    }

    esp_capture_audio_src_if_t* gated_source =
        build_gated_audio_source(s_afe_capture->CaptureSource());
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
        // because EidolonAfeCapture reads mic+reference PCM through it.
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

extern "C" int eidolon_livekit_board_pull_playback_reference(int16_t* out, int num_samples)
{
    if (out == nullptr || num_samples <= 0) {
        return 0;
    }
    const size_t w = s_ref_write;
    const size_t need = kRefDelaySamples + static_cast<size_t>(num_samples);
    // Feed the `num_samples` that were played ~kRefDelaySamples ago (anchored to
    // the write head each call, so it stays delay-locked even if producer/consumer
    // rates jitter — no free-running drift). Idle playback (no recent push) → no
    // echo to cancel → silent reference.
    const bool playing = (esp_timer_get_time() - s_last_playback_us) < 150 * 1000;
    if (playing && w >= need) {
        const size_t start = w - need;  // == (w - kRefDelaySamples) - num_samples
        for (int i = 0; i < num_samples; ++i) {
            out[i] = s_ref_ring[(start + static_cast<size_t>(i)) & (kRefRingSamples - 1)];
        }
        return num_samples;
    }
    for (int i = 0; i < num_samples; ++i) {
        out[i] = 0;
    }
    return 0;
}

extern "C" bool eidolon_livekit_board_near_end_active(void)
{
    return s_afe_capture != nullptr && s_afe_capture->NearEndActiveRecently();
}

extern "C" esp_err_t eidolon_livekit_board_set_capture_enabled(bool enabled)
{
    if (!s_capturer) {
        return ESP_ERR_INVALID_STATE;
    }
    bool previous = s_gated_audio_source.enabled;
    s_gated_audio_source.enabled = enabled;
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
    if (s_capturer) {
        esp_capture_close(s_capturer);
        s_capturer = nullptr;
    }
    // Keep the EidolonAfeCapture instance alive across sessions: its embedded
    // AfeAudioProcessor owns a long-lived AFE task that cannot be safely torn
    // down. Stop() idles it; build_capturer reuses the same instance next time.
    if (s_afe_capture) {
        s_afe_capture->Stop();
    }
    s_gated_audio_source = {};
    s_last_playback_us = 0;
}
