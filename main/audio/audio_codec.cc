#include "audio_codec.h"
#include "board.h"
#include "settings.h"
#include "eidolon/audio/output_drain.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}


esp_err_t AudioCodec::DrainOutput(const std::function<bool()>& current) {
    if (!tx_handle_ || !current) return ESP_ERR_INVALID_STATE;
    i2s_chan_info_t info{};
    esp_err_t error = i2s_channel_get_info(tx_handle_, &info);
    if (error != ESP_OK) return error;
    if (info.dir != I2S_DIR_TX || !info.total_dma_buf_size) return ESP_ERR_NOT_SUPPORTED;
    const bool drained = eidolon::DrainAudioOutput(info.total_dma_buf_size,
        [this, &error](const uint8_t* data, size_t size, size_t& written) {
            // A revoked reply need not wait for a whole drain to finish: the
            // fence is rechecked between small writes, each bounded to 20 ms.
            error = i2s_channel_write(tx_handle_, data, size, &written, 20);
            return error == ESP_OK;
        }, current);
    return drained ? ESP_OK : (error != ESP_OK ? error : ESP_ERR_INVALID_STATE);
}
