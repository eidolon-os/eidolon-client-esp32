#include "micro_wake_word_detector.h"

#include "eidolon_mww.h"
#include <esp_log.h>

#define TAG "EidolonMwwDet"

bool MicroWakeWordDetector::Initialize(AudioCodec* codec)
{
    codec_ = codec;
    (void)codec_;

    eidolon_mww_config_t cfg = {
        .probability_cutoff = static_cast<uint8_t>(CONFIG_EIDOLON_WAKE_WORD_THRESHOLD),
        .sliding_window_size = 10,
        .cooldown_ms = CONFIG_EIDOLON_WAKE_WORD_COOLDOWN_MS,
    };
    if (eidolon_mww_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "eidolon_mww_init failed");
        return false;
    }
    return true;
}

void MicroWakeWordDetector::OnWakeWordDetected(
    std::function<void(const std::string& wake_word)> callback)
{
    callback_ = std::move(callback);
}

void MicroWakeWordDetector::Start()
{
    running_ = true;
    eidolon_mww_reset();
}

void MicroWakeWordDetector::Stop()
{
    running_ = false;
}

size_t MicroWakeWordDetector::GetFeedSize()
{
    return 160;
}

void MicroWakeWordDetector::Feed(const std::vector<int16_t>& data)
{
    if (!running_ || data.empty()) {
        return;
    }

    eidolon_mww_feed_pcm(data.data(), data.size());

    if (eidolon_mww_poll_detected()) {
        last_wake_word_ = "hey_jarvis";
        ESP_LOGI(TAG, "Wake word detected: %s", last_wake_word_.c_str());
        if (callback_) {
            callback_(last_wake_word_);
        }
    }
}
