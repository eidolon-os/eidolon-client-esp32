#include "eidolon_audio_input_service.h"

#include "eidolon_audio_input.h"
#include "wake_word/micro_wake_word_detector.h"

#include <esp_log.h>

#define TAG "EidolonWakeWord"

EidolonAudioInputService::EidolonAudioInputService()
{
    event_group_ = xEventGroupCreate();
    wake_word_ = std::make_unique<MicroWakeWordDetector>();
}

EidolonAudioInputService::~EidolonAudioInputService()
{
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void EidolonAudioInputService::Initialize(AudioCodec* codec)
{
    codec_ = codec;
}

void EidolonAudioInputService::SetCallbacks(EidolonAudioInputCallbacks& callbacks)
{
    callbacks_ = callbacks;
}

void EidolonAudioInputService::Start()
{
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, EAS_EVENT_WAKE_WORD_RUNNING);

    xTaskCreatePinnedToCore(
        [](void* arg) {
            static_cast<EidolonAudioInputService*>(arg)->AudioInputTask();
            vTaskDelete(nullptr);
        },
        "eidolon_audio_in", 4096, this, 8, &audio_input_task_handle_, 0);
}

void EidolonAudioInputService::Stop()
{
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, EAS_EVENT_WAKE_WORD_RUNNING);
}

bool EidolonAudioInputService::IsWakeWordRunning() const
{
    return xEventGroupGetBits(event_group_) & EAS_EVENT_WAKE_WORD_RUNNING;
}

bool EidolonAudioInputService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples)
{
    if (!codec_) {
        return false;
    }

    auto& platform = eidolon::EidolonAudioInput::Instance();
    if (!platform.IsReady()) {
        return false;
    }

    if (sample_rate != 16000) {
        ESP_LOGW(TAG, "Wake word expects 16 kHz input");
        return false;
    }

    data.resize(samples);
    if (!platform.ReadMonoPcm16k(data.data(), samples)) {
        return false;
    }
    return true;
}

void EidolonAudioInputService::AudioInputTask()
{
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, EAS_EVENT_WAKE_WORD_RUNNING, pdFALSE,
                                               pdFALSE, portMAX_DELAY);
        if (service_stopped_) {
            break;
        }

        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (bits & EAS_EVENT_WAKE_WORD_RUNNING) {
            const int samples = 160;  // 10 ms @ 16 kHz
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                wake_word_->Feed(data);
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void EidolonAudioInputService::EnableWakeWordDetection(bool enable)
{
    if (!wake_word_) {
        return;
    }

    ESP_LOGI(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
            wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
                if (callbacks_.on_wake_word_detected) {
                    callbacks_.on_wake_word_detected(wake_word);
                }
            });
        }
        wake_word_->Start();
        audio_input_need_warmup_ = true;
        xEventGroupSetBits(event_group_, EAS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, EAS_EVENT_WAKE_WORD_RUNNING);
    }
}
