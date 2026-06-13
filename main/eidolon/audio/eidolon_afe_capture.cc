#include "eidolon_afe_capture.h"

#include "audio_codec.h"
#include "processors/afe_audio_processor.h"

#include <esp_log.h>

#include <utility>
#include <vector>

#define TAG "EidolonAfeCap"

namespace {
// AFE output framing. The PcmPushCaptureSource ring decouples this from the
// encoder pull size, so the exact value only affects callback granularity.
constexpr int kAfeFrameDurationMs = 20;
// 10 ms mic read chunk @ 16 kHz, matching the xiaozhi AudioInputTask cadence.
constexpr int kReadFramesPerChannel = 160;
}  // namespace

namespace eidolon {

EidolonAfeCapture::EidolonAfeCapture() = default;

EidolonAfeCapture::~EidolonAfeCapture() {
    Stop();
}

esp_err_t EidolonAfeCapture::Start(AudioCodec* codec) {
    if (running_) {
        return ESP_OK;
    }
    if (codec == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    codec_ = codec;
    codec_->Start();
    codec_->EnableInput(true);

    afe_ = std::make_unique<AfeAudioProcessor>();
    // nullptr models -> AfeAudioProcessor loads the on-flash "model" partition.
    afe_->Initialize(codec_, kAfeFrameDurationMs, nullptr);
    afe_->OnOutput([this](std::vector<int16_t>&& data) {
        pcm_source_.Push(data.data(), data.size());
    });

    running_ = true;
    afe_->Start();

    BaseType_t created = xTaskCreate([](void* arg) {
        static_cast<EidolonAfeCapture*>(arg)->ReadLoop();
        vTaskDelete(nullptr);
    }, "eidolon_afe_rd", 4096, this, 5, &read_task_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE read task");
        running_ = false;
        afe_->Stop();
        afe_.reset();
        read_task_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE capture started (channels=%d, %d Hz)", codec_->input_channels(),
             codec_->input_sample_rate());
    return ESP_OK;
}

void EidolonAfeCapture::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    // The read loop polls running_ and clears read_task_ before exiting.
    for (int i = 0; i < 100 && read_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (afe_) {
        afe_->Stop();
        afe_.reset();
    }
    ESP_LOGI(TAG, "AFE capture stopped");
}

void EidolonAfeCapture::ReadLoop() {
    const int channels = codec_->input_channels();
    std::vector<int16_t> data;
    while (running_) {
        data.resize(static_cast<size_t>(kReadFramesPerChannel) * channels);
        if (!codec_->InputData(data)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        afe_->Feed(std::move(data));
    }
    read_task_ = nullptr;
}

}  // namespace eidolon
