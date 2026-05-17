#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <memory>
#include <mutex>
#include <vector>

#include "eidolon_wake_word_detector.h"

class AudioCodec;

struct EidolonAudioInputCallbacks {
    std::function<void(const std::string&)> on_wake_word_detected;
};

#define EAS_EVENT_WAKE_WORD_RUNNING (1 << 0)

class EidolonAudioInputService {
public:
    EidolonAudioInputService();
    ~EidolonAudioInputService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void SetCallbacks(EidolonAudioInputCallbacks& callbacks);
    void EnableWakeWordDetection(bool enable);
    bool IsWakeWordRunning() const;

private:
    AudioCodec* codec_ = nullptr;
    EidolonAudioInputCallbacks callbacks_;
    std::unique_ptr<EidolonWakeWordDetector> wake_word_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t audio_input_task_handle_ = nullptr;
    bool service_stopped_ = true;
    bool wake_word_initialized_ = false;
    bool audio_input_need_warmup_ = false;

    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void AudioInputTask();
};
