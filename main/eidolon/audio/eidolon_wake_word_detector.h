#pragma once

#include <functional>
#include <string>
#include <vector>

class AudioCodec;

class EidolonWakeWordDetector {
public:
    virtual ~EidolonWakeWordDetector() = default;

    virtual bool Initialize(AudioCodec* codec) = 0;
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual size_t GetFeedSize() = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};
