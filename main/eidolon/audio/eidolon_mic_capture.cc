#include "eidolon_mic_capture.h"

#include "audio_codec.h"
#include "eidolon_audio_input.h"
#if CONFIG_USE_DEVICE_AEC
#include "processors/afe_audio_processor.h"
#endif

#include <esp_log.h>

#include <utility>
#include <vector>

#define TAG "EidolonMicCap"

namespace {
// AFE output framing. The PcmPushCaptureSource ring decouples this from the
// encoder pull size, so the exact value only affects callback granularity.
constexpr int kAfeFrameDurationMs = 20;
// 10 ms mic read chunk @ 16 kHz, matching the xiaozhi AudioInputTask cadence.
constexpr int kReadFramesPerChannel = 160;
#if CONFIG_USE_DEVICE_AEC
// The read loop also drives AfeAudioProcessor::Feed (ESP-SR AFE feed in the
// VC/HIGH_PERF profile), which is stack-heavy; 4 KB overflows. xiaozhi's
// equivalent input task uses ~6 KB — give headroom.
constexpr int kReadTaskStack = 8192;
#else
// Raw topology: the loop only reads the codec and pushes into the ring, no ESP-SR
// frame on the stack. 3 KB is ample and returns ~5 KB of internal SRAM, which on
// the boards that take this path is the difference between a voice JOIN building
// the LiveKit engine and failing with "Failed to create engine".
constexpr int kReadTaskStack = 3072;
#endif
// Discard the first ~150 ms of mic after start so the codec DMA / AEC settle
// before we publish or feed anything (mirrors xiaozhi's audio-input warmup).
constexpr int kWarmupFrames = 15;
}  // namespace

namespace eidolon {

EidolonMicCapture::EidolonMicCapture() = default;

EidolonMicCapture::~EidolonMicCapture() {
    Stop();
}

esp_err_t EidolonMicCapture::Start(AudioCodec* codec) {
    if (running_) {
        return ESP_OK;
    }
    if (codec == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    codec_ = codec;
    codec_->Start();
    codec_->EnableInput(true);

#if CONFIG_USE_DEVICE_AEC
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
#else
    running_ = true;
#endif

    // Pinned to core 0 at priority 8, matching xiaozhi's audio-input task.
    BaseType_t created = xTaskCreatePinnedToCore([](void* arg) {
        static_cast<EidolonMicCapture*>(arg)->ReadLoop();
        vTaskDelete(nullptr);
    }, "eidolon_mic_rd", kReadTaskStack, this, 8, &read_task_, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic read task");
        running_ = false;
#if CONFIG_USE_DEVICE_AEC
        afe_.reset();  // safe: joins the internal task
#endif
        read_task_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Mic capture started topology=%s (codec=%d Hz, out=16000 Hz, channels=%d)",
             kCaptureViaAfe ? "afe" : "raw",
             codec_->input_sample_rate(), codec_->input_channels());
    return ESP_OK;
}

void EidolonMicCapture::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    // The read loop polls running_ and clears read_task_ before exiting. Stop the
    // reader first so no Feed()/Push() runs concurrently with the teardown below.
    for (int i = 0; i < 100 && read_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#if CONFIG_USE_DEVICE_AEC
    // Destroy the AFE for this session. Safe: ~AfeAudioProcessor stops and joins
    // its internal fetch task before freeing afe_data_.
    if (afe_) {
        afe_->Stop();
        afe_.reset();
    }
#endif
    // Quiesce the PCM source that esp_capture's fetch thread pulls from, so that
    // fetch thread unblocks and exits BEFORE the caller tears down the capture
    // pipeline (esp_capture_close). Without this it stays parked in ReadFrame and
    // esp_capture frees its data queue out from under it -> use-after-free.
    pcm_source_.Quiesce();
    ESP_LOGI(TAG, "Mic capture stopped");
}

void EidolonMicCapture::ReadLoop() {
#if CONFIG_USE_DEVICE_AEC
    auto& audio_input = EidolonAudioInput::Instance();
    int warmup = kWarmupFrames;
    std::vector<int16_t> data;
    while (running_) {
        // Interleaved: the AFE needs the reference channel alongside the mic.
        if (!audio_input.ReadInterleavedPcm16k(data, kReadFramesPerChannel)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (warmup > 0) {
            --warmup;  // drain but don't feed yet (let codec/AEC settle)
            continue;
        }
        afe_->Feed(std::move(data));
    }
#else
    auto& audio_input = EidolonAudioInput::Instance();
    int warmup = kWarmupFrames;
    // Mono: ReadMonoPcm16k takes channel 0, so a board that still declares an
    // (unusable) reference channel simply has it dropped here.
    int16_t mono[kReadFramesPerChannel];
    while (running_) {
        if (!audio_input.ReadMonoPcm16k(mono, kReadFramesPerChannel)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (warmup > 0) {
            --warmup;  // drain but don't publish yet (let the codec DMA settle)
            continue;
        }
        pcm_source_.Push(mono, kReadFramesPerChannel);
    }
#endif
    read_task_ = nullptr;
}

}  // namespace eidolon
