// LiveKit media board bring-up for Waveshare ESP32-S3-Touch-AMOLED-2.06 (shared BoxAudioCodec).

#include "livekit_board.h"

#include "audio_codec.h"
#include "eidolon_audio_input.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_enc_default.h>
#include <esp_capture_defaults.h>
#include <esp_capture_sink.h>
#include <esp_check.h>
#include <esp_codec_dev.h>
#include <esp_log.h>
#include <av_render_default.h>

#define TAG "EidolonLKBoard"

#define LIVEKIT_I2S_SAMPLE_RATE 16000
#define LIVEKIT_SPEAKER_VOLUME CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME

static esp_capture_sink_handle_t s_capturer;
static esp_capture_audio_src_if_t* s_audio_source;
static audio_render_handle_t s_audio_renderer;
static av_render_handle_t s_av_renderer;

static esp_err_t build_capturer(esp_codec_dev_handle_t record_handle)
{
    esp_capture_audio_aec_src_cfg_t aec_cfg = {
        .record_handle = record_handle,
        .channel = 4,
        .channel_mask = 1 | 2,
    };
    s_audio_source = esp_capture_new_audio_aec_src(&aec_cfg);
    if (!s_audio_source) {
        return ESP_FAIL;
    }

    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_audio_source,
    };
    return esp_capture_open(&cfg, &s_capturer);
}

static esp_err_t build_renderer(esp_codec_dev_handle_t play_handle)
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = play_handle,
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
    frame_info.sample_rate = LIVEKIT_I2S_SAMPLE_RATE;
    frame_info.channel = 2;
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

    esp_codec_dev_handle_t rec = audio_in.RecordHandle();
    esp_codec_dev_handle_t play = audio_in.PlaybackHandle();
    if (!rec || !play) {
        ESP_LOGE(TAG, "Codec record/play handles not available");
        return ESP_ERR_INVALID_STATE;
    }

    auto* codec = audio_in.Codec();
    if (codec) {
        std::lock_guard<std::mutex> lock(audio_in.Mutex());
        codec->SetOutputVolume(LIVEKIT_SPEAKER_VOLUME);
        codec->EnableOutput(true);
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    ESP_RETURN_ON_ERROR(build_capturer(rec), TAG, "capturer");
    ESP_RETURN_ON_ERROR(build_renderer(play), TAG, "renderer");

    ESP_LOGI(TAG, "LiveKit board media ready (shared codec @ %d Hz)", LIVEKIT_I2S_SAMPLE_RATE);
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
    s_audio_source = nullptr;

    auto* codec = eidolon::EidolonAudioInput::Instance().Codec();
    if (codec) {
        codec->EnableOutput(false);
    }
}
