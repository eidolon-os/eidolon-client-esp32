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
// The read loop also drives AfeAudioProcessor::Feed (ESP-SR AFE feed in the
// VC/HIGH_PERF profile), which is stack-heavy; 4 KB overflows. xiaozhi's
// equivalent input task uses ~6 KB — give headroom.
constexpr int kReadTaskStack = 8192;
// Discard the first ~150 ms of mic after start so the codec DMA / AEC settle
// before we feed the AFE (mirrors xiaozhi's audio-input warmup).
constexpr int kWarmupFrames = 15;
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

    // Fresh AFE per session: avoids carrying stale internal state across
    // start/stop (which could fault in afe_feed/afe_parse_input). Safe now that
    // AfeAudioProcessor's destructor stops and joins its internal task before
    // freeing afe_data_.
    afe_ = std::make_unique<AfeAudioProcessor>();
    // nullptr models -> AfeAudioProcessor loads the on-flash "model" partition.
    afe_->Initialize(codec_, kAfeFrameDurationMs, nullptr);
    afe_->OnOutput([this](std::vector<int16_t>&& data) {
        pcm_source_.Push(data.data(), data.size());
    });

    running_ = true;
    afe_->Start();
    // Enable device AEC (disables VAD): the AFE cancels echo against the codec's
    // hardware reference channel before the cleaned PCM is published.
    afe_->EnableDeviceAec(true);

    // Pinned to core 0 at priority 8, matching xiaozhi's audio-input task.
    BaseType_t created = xTaskCreatePinnedToCore([](void* arg) {
        static_cast<EidolonAfeCapture*>(arg)->ReadLoop();
        vTaskDelete(nullptr);
    }, "eidolon_afe_rd", kReadTaskStack, this, 8, &read_task_, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE read task");
        running_ = false;
        afe_.reset();  // safe: joins the internal task
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
    // The read loop polls running_ and clears read_task_ before exiting. Stop the
    // reader first so no Feed() runs concurrently with afe_->Stop().
    for (int i = 0; i < 100 && read_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Destroy the AFE for this session. Safe: ~AfeAudioProcessor stops and joins
    // its internal fetch task before freeing afe_data_.
    if (afe_) {
        afe_->Stop();
        afe_.reset();
    }
    ESP_LOGI(TAG, "AFE capture stopped");
}

void EidolonAfeCapture::ReadLoop() {
    const int channels = codec_->input_channels();
    int warmup = kWarmupFrames;
    std::vector<int16_t> data;
    while (running_) {
        data.resize(static_cast<size_t>(kReadFramesPerChannel) * channels);
        if (!codec_->InputData(data)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (warmup > 0) {
            --warmup;  // drain but don't feed yet (let codec/AEC settle)
            continue;
        }
        afe_->Feed(std::move(data));
    }
    read_task_ = nullptr;
}

}  // namespace eidolon
