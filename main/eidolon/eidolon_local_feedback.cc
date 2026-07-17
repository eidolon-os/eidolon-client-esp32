#include "eidolon_local_feedback.h"

#include "audio_codec.h"
#include "board.h"
#include "eidolon_audio_input.h"

#include <cmath>
#include <cstdint>
#include <esp_log.h>
#include <mutex>
#include <vector>

#define TAG "EidolonFeedback"

namespace eidolon {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kIdentifyToneAmplitude = 0.28f;
constexpr int kIdentifyToneFadeMs = 8;

void AppendTone(std::vector<int16_t>& pcm, int sample_rate, float frequency_hz,
                int duration_ms, float amplitude)
{
    const size_t start = pcm.size();
    const size_t samples = static_cast<size_t>(sample_rate) * duration_ms / 1000;
    const size_t fade_samples = static_cast<size_t>(sample_rate) * kIdentifyToneFadeMs / 1000;
    pcm.resize(start + samples);

    for (size_t i = 0; i < samples; ++i) {
        float envelope = 1.0f;
        if (fade_samples > 0 && i < fade_samples) {
            envelope = static_cast<float>(i) / static_cast<float>(fade_samples);
        } else if (fade_samples > 0 && i + fade_samples >= samples) {
            envelope = static_cast<float>(samples - i) / static_cast<float>(fade_samples);
        }
        const float phase = 2.0f * kPi * frequency_hz * static_cast<float>(i) /
                            static_cast<float>(sample_rate);
        pcm[start + i] = static_cast<int16_t>(std::sin(phase) * amplitude * envelope * 32767.0f);
    }
}

void AppendSilence(std::vector<int16_t>& pcm, int sample_rate, int duration_ms)
{
    pcm.resize(pcm.size() + static_cast<size_t>(sample_rate) * duration_ms / 1000);
}

std::vector<int16_t> BuildIdentifyTone(int sample_rate)
{
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(sample_rate) * 420 / 1000);
    AppendTone(pcm, sample_rate, 740.0f, 140, kIdentifyToneAmplitude);
    AppendSilence(pcm, sample_rate, 70);
    AppendTone(pcm, sample_rate, 980.0f, 160, kIdentifyToneAmplitude);
    return pcm;
}

esp_err_t PlayIdentifyFeedbackLocked(AudioCodec* codec)
{
    if (codec == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    codec->Start();
    const bool was_output_enabled = codec->output_enabled();
    if (!was_output_enabled) {
        codec->EnableOutput(true);
    }

    const int sample_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    auto pcm = BuildIdentifyTone(sample_rate);
    codec->OutputData(pcm);

    if (!was_output_enabled) {
        codec->EnableOutput(false);
    }
    ESP_LOGI(TAG, "Identify feedback played locally (%d Hz, %zu samples)", sample_rate, pcm.size());
    return ESP_OK;
}

}  // namespace

esp_err_t PlayIdentifyFeedback()
{
    auto& audio = EidolonAudioInput::Instance();
    if (audio.IsReady()) {
        std::lock_guard<std::mutex> lock(audio.Mutex());
        return PlayIdentifyFeedbackLocked(audio.Codec());
    }

    return PlayIdentifyFeedbackLocked(Board::GetInstance().GetAudioCodec());
}

esp_err_t PlayRollCallFeedback()
{
    // V1 uses the existing local two-tone cue as Guard's audible "I am here"
    // response. The capability contract stays stable if a spoken asset replaces
    // the cue later.
    esp_err_t err = PlayIdentifyFeedback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Roll-call feedback played locally");
    }
    return err;
}

}  // namespace eidolon
