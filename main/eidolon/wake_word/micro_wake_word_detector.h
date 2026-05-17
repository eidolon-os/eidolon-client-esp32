#pragma once

#include "eidolon/audio/eidolon_wake_word_detector.h"

#include <string>

class MicroWakeWordDetector : public EidolonWakeWordDetector {
public:
    bool Initialize(AudioCodec* codec) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    const std::string& GetLastDetectedWakeWord() const override { return last_wake_word_; }

private:
    AudioCodec* codec_ = nullptr;
    std::function<void(const std::string&)> callback_;
    std::string last_wake_word_;
    bool running_ = false;
};
