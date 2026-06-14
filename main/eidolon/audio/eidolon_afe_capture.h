#pragma once

#include "pcm_push_capture_source.h"

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

class AudioCodec;
class AfeAudioProcessor;

namespace eidolon {

// Device-side AEC capture pipeline for the LiveKit voice path.
//
// Reuses the proven xiaozhi `AfeAudioProcessor` (ESP-SR AFE in the voice-comm
// profile: AFE_TYPE_VC + AEC_MODE_VOIP_HIGH_PERF) instead of esp_capture's
// wake-word-tuned `esp_capture_audio_aec_src`. A dedicated task reads raw
// interleaved mic+reference PCM from the codec, feeds the AFE, and the AFE's
// echo-cancelled mono output is pushed into a `PcmPushCaptureSource` that the
// LiveKit publisher pulls from.
//
// AEC reference: the AFE consumes the codec's hardware reference channel (the
// trailing 'R' in the "MR" input layout, i.e. the speaker loopback captured by
// the ES7210). See the AEC architecture section of the device pairing doc.
class EidolonAfeCapture {
public:
    // Constructor and destructor are out-of-line so the unique_ptr<AfeAudioProcessor>
    // member is only instantiated where AfeAudioProcessor is a complete type
    // (the forward declaration above keeps the header light for callers).
    EidolonAfeCapture();
    ~EidolonAfeCapture();

    EidolonAfeCapture(const EidolonAfeCapture&) = delete;
    EidolonAfeCapture& operator=(const EidolonAfeCapture&) = delete;

    // Start the AFE pipeline against `codec` (must outlive this object).
    esp_err_t Start(AudioCodec* codec);
    void Stop();
    bool IsRunning() const { return running_; }

    // esp_capture audio source the LiveKit publisher reads cancelled PCM from.
    esp_capture_audio_src_if_t* CaptureSource() { return pcm_source_.Interface(); }

    // True when the AFE detected near-end (real user) speech on the AEC-cleaned
    // signal within a short hangover window. Used to tell echo from genuine
    // barge-in during agent playback.
    bool NearEndActiveRecently() const;

private:
    void ReadLoop();
    void OnVadState(bool speaking);

    PcmPushCaptureSource pcm_source_;
    std::unique_ptr<AfeAudioProcessor> afe_;
    AudioCodec* codec_ = nullptr;
    TaskHandle_t read_task_ = nullptr;
    volatile bool running_ = false;
    volatile int64_t last_near_end_us_ = 0;
    // Energy-gate state for near-end detection on the AEC-cleaned output (OnOutput).
    double rms_sumsq_ = 0.0;
    int rms_count_ = 0;
    int near_end_run_ = 0;   // consecutive windows over threshold (persistence)
    int rms_log_div_ = 0;    // throttles the diagnostic RMS log
};

}  // namespace eidolon
