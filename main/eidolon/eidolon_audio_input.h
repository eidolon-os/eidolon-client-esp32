#pragma once

#include <esp_codec_dev.h>
#include <esp_err.h>
#include <mutex>
#include <vector>

class AudioCodec;

namespace eidolon {

class EidolonAudioInput {
public:
    static EidolonAudioInput& Instance();

    esp_err_t Init();
    bool IsReady() const { return ready_; }

    std::mutex& Mutex() { return mutex_; }
    AudioCodec* Codec() const { return codec_; }
    esp_codec_dev_handle_t RecordHandle() const;
    esp_codec_dev_handle_t PlaybackHandle() const;

    /**
     * Read 16 kHz interleaved PCM while preserving all codec input channels.
     * `frames` is the number of per-channel samples requested.
     */
    bool ReadInterleavedPcm16k(std::vector<int16_t>& data, size_t frames);

    /** Read mono 16 kHz PCM (samples = number of int16 mono samples). */
    bool ReadMonoPcm16k(int16_t* buf, size_t samples);

private:
    EidolonAudioInput() = default;

    std::mutex mutex_;
    AudioCodec* codec_ = nullptr;
    bool ready_ = false;
    void* resampler_ = nullptr;
    std::mutex resampler_mutex_;
    std::vector<int16_t> resample_input_buffer_;
};

}  // namespace eidolon
