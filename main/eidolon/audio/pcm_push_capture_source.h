#pragma once

#include <esp_capture_audio_src_if.h>
#include <esp_capture_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <cstddef>
#include <cstdint>

namespace eidolon {

// Custom esp_capture audio source that the application pushes already-processed
// (AEC'd) 16 kHz mono 16-bit PCM into. esp_capture / the LiveKit publisher pulls
// frames through the standard esp_capture_audio_src_if_t vtable.
//
// Rationale: the upstream esp_capture_audio_aec_src runs ESP-SR AFE in the
// wake-word profile (AFE_TYPE_SR), which is poorly tuned for full-duplex voice
// and gives bad echo cancellation. Instead we reuse the proven xiaozhi
// AfeAudioProcessor (AFE_TYPE_VC + AEC_MODE_VOIP_HIGH_PERF) to produce clean PCM
// and feed it here, so LiveKit only transports already-cancelled audio.
//
// An internal byte ring (FreeRTOS stream buffer) decouples the AFE output frame
// size from the encoder pull size. Single-writer (AFE task) / single-reader
// (capture fetch task), which the stream buffer supports without extra locking.
class PcmPushCaptureSource {
public:
    explicit PcmPushCaptureSource(uint32_t sample_rate = 16000);
    ~PcmPushCaptureSource();

    PcmPushCaptureSource(const PcmPushCaptureSource&) = delete;
    PcmPushCaptureSource& operator=(const PcmPushCaptureSource&) = delete;

    // Stable interface pointer for esp_capture_cfg_t.audio_src. `base_` is the
    // first member so the vtable pointer aliases `this`.
    esp_capture_audio_src_if_t* Interface() { return &base_; }

    // Push processed mono int16 PCM. Non-blocking; on overflow the excess is
    // dropped (logged, throttled) rather than blocking the AFE task.
    void Push(const int16_t* samples, size_t sample_count);

    // Drop any buffered audio (e.g. on playback flush / session reset).
    void Flush();

private:
    static PcmPushCaptureSource* From(esp_capture_audio_src_if_t* h);

    static esp_capture_err_t Open(esp_capture_audio_src_if_t* h);
    static esp_capture_err_t GetSupportCodecs(esp_capture_audio_src_if_t* h,
                                              const esp_capture_format_id_t** codecs, uint8_t* num);
    static esp_capture_err_t NegotiateCaps(esp_capture_audio_src_if_t* h,
                                           esp_capture_audio_info_t* in_caps,
                                           esp_capture_audio_info_t* out_caps);
    static esp_capture_err_t Start(esp_capture_audio_src_if_t* h);
    static esp_capture_err_t ReadFrame(esp_capture_audio_src_if_t* h,
                                       esp_capture_stream_frame_t* frame);
    static esp_capture_err_t Stop(esp_capture_audio_src_if_t* h);
    static esp_capture_err_t Close(esp_capture_audio_src_if_t* h);

    esp_capture_audio_src_if_t base_{};  // MUST be first member.
    StreamBufferHandle_t ring_ = nullptr;
    uint32_t sample_rate_;
    volatile bool running_ = false;
    uint64_t samples_read_ = 0;
};

}  // namespace eidolon
