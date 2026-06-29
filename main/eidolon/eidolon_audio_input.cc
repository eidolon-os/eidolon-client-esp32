#include "eidolon_audio_input.h"

#include "audio_codec.h"
#include "board.h"
#include "codecs/box_audio_codec.h"
#include "codecs/es8388_audio_codec.h"

#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_log.h>

#define TAG "EidolonAudioIn"

namespace eidolon {

EidolonAudioInput& EidolonAudioInput::Instance()
{
    static EidolonAudioInput instance;
    return instance;
}

esp_codec_dev_handle_t EidolonAudioInput::RecordHandle() const
{
    auto* box = dynamic_cast<BoxAudioCodec*>(codec_);
    if (box) {
        return box->GetInputDeviceHandle();
    }
    auto* es8388 = dynamic_cast<Es8388AudioCodec*>(codec_);
    return es8388 ? es8388->GetInputDeviceHandle() : nullptr;
}

esp_codec_dev_handle_t EidolonAudioInput::PlaybackHandle() const
{
    auto* box = dynamic_cast<BoxAudioCodec*>(codec_);
    if (box) {
        return box->GetOutputDeviceHandle();
    }
    auto* es8388 = dynamic_cast<Es8388AudioCodec*>(codec_);
    return es8388 ? es8388->GetOutputDeviceHandle() : nullptr;
}

esp_err_t EidolonAudioInput::Init()
{
    if (ready_) {
        return ESP_OK;
    }

    codec_ = Board::GetInstance().GetAudioCodec();
    if (!codec_) {
        ESP_LOGE(TAG, "No audio codec from board");
        return ESP_ERR_NOT_FOUND;
    }

    codec_->Start();
    codec_->EnableInput(true);

    if (codec_->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate = static_cast<uint32_t>(codec_->input_sample_rate()),
            .dest_rate = 16000,
            .channel = static_cast<uint8_t>(codec_->input_channels()),
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
        };
        esp_ae_rate_cvt_handle_t handle = nullptr;
        if (esp_ae_rate_cvt_open(&cfg, &handle) != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open input resampler");
            return ESP_FAIL;
        }
        resampler_ = handle;
        ESP_LOGI(TAG, "Input resampler %d -> 16000 Hz", codec_->input_sample_rate());
    }

    ready_ = true;
    ESP_LOGI(TAG, "audio platform ready");
    return ESP_OK;
}

bool EidolonAudioInput::ReadInterleavedPcm16k(std::vector<int16_t>& data, size_t frames)
{
    if (!ready_ || !codec_ || frames == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!codec_->input_enabled()) {
        codec_->EnableInput(true);
    }

    const int channels = codec_->input_channels();
    if (channels <= 0) {
        return false;
    }

    if (codec_->input_sample_rate() == 16000) {
        data.resize(frames * channels);
        return codec_->InputData(data);
    }

    const size_t in_frames =
        (frames * static_cast<size_t>(codec_->input_sample_rate()) + 15999) / 16000;
    resample_input_buffer_.resize(in_frames * channels);
    if (!codec_->InputData(resample_input_buffer_)) {
        return false;
    }
    if (!resampler_) {
        return false;
    }

    std::lock_guard<std::mutex> rlock(resampler_mutex_);
    uint32_t in_count = resample_input_buffer_.size() / channels;
    uint32_t out_max = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(
        static_cast<esp_ae_rate_cvt_handle_t>(resampler_), in_count, &out_max);
    data.resize(out_max * channels);
    uint32_t actual_out = out_max;
    esp_ae_rate_cvt_process(static_cast<esp_ae_rate_cvt_handle_t>(resampler_),
                            reinterpret_cast<esp_ae_sample_t*>(resample_input_buffer_.data()),
                            in_count, reinterpret_cast<esp_ae_sample_t*>(data.data()),
                            &actual_out);
    data.resize(actual_out * channels);
    return !data.empty();
}

bool EidolonAudioInput::ReadMonoPcm16k(int16_t* buf, size_t samples)
{
    if (!ready_ || !codec_ || !buf || samples == 0) {
        return false;
    }

    std::vector<int16_t> raw;
    if (!ReadInterleavedPcm16k(raw, samples)) {
        return false;
    }

    const int channels = codec_->input_channels();
    if (channels <= 0) {
        return false;
    }

    const size_t frames = raw.size() / channels;
    if (frames < samples) {
        return false;
    }
    for (size_t i = 0; i < samples; ++i) {
        buf[i] = raw[i * channels];
    }
    return true;
}

}  // namespace eidolon
