#pragma once

#include "../eidolon_device_profile.h"
#include "pcm_push_capture_source.h"

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

class AudioCodec;
class AfeAudioProcessor;

namespace eidolon {

// Microphone capture for the LiveKit voice path, in one of two topologies chosen
// at compile time by the board's CAPABILITY (never by its interaction mode — see
// eidolon_device_profile.h):
//
//   kCaptureViaAfe (CONFIG_USE_DEVICE_AEC=y, e.g. esp-box-3)
//     A dedicated task reads raw interleaved mic+reference PCM from the codec and
//     feeds the proven xiaozhi `AfeAudioProcessor` (ESP-SR AFE in the voice-comm
//     profile, AFE_TYPE_VC) instead of esp_capture's wake-word-tuned
//     `esp_capture_audio_aec_src`. The AFE's echo-cancelled mono output is pushed
//     into the `PcmPushCaptureSource` the LiveKit publisher pulls from. The
//     reference is the codec's hardware loopback channel (the trailing 'R' in the
//     "MR" input layout, captured by the ES7210) — see the AEC architecture
//     section of the device pairing doc.
//
//   raw (CONFIG_USE_DEVICE_AEC=n, e.g. waveshare-2.06 ptt, m5stack-stackchan
//     half_duplex)
//     The same task reads the mic and pushes it straight into the ring. Running
//     the AFE here would cancel nothing (no aec_init, no NS model flashed) and its
//     VAD output has no consumer on this path, so it would only spend internal
//     SRAM, a task and CPU to copy PCM. `ReadMonoPcm16k` takes channel 0, so a
//     board that still declares AUDIO_INPUT_REFERENCE=true keeps working — the
//     unused reference slot is simply dropped.
//
// Identical in both: this class opens the codec, discards the first ~150 ms so the
// codec DMA settles, and the ring decouples read cadence from the encoder's pull
// size. Mic GATING (ptt held / half_duplex not-while-playing / full_duplex open)
// is not done here at all — it lives in the gated capture source in
// livekit_board.cc, so all three modes work with either topology.
class EidolonMicCapture {
public:
    // Constructor and destructor are out-of-line so the unique_ptr<AfeAudioProcessor>
    // member is only instantiated where AfeAudioProcessor is a complete type
    // (the forward declaration above keeps the header light for callers).
    EidolonMicCapture();
    ~EidolonMicCapture();

    EidolonMicCapture(const EidolonMicCapture&) = delete;
    EidolonMicCapture& operator=(const EidolonMicCapture&) = delete;

    // Start capturing from `codec` (must outlive this object).
    esp_err_t Start(AudioCodec* codec);
    void Stop();
    bool IsRunning() const { return running_; }

    // esp_capture audio source the LiveKit publisher reads mic PCM from.
    esp_capture_audio_src_if_t* CaptureSource() { return pcm_source_.Interface(); }

private:
    void ReadLoop();

    PcmPushCaptureSource pcm_source_;
#if CONFIG_USE_DEVICE_AEC
    std::unique_ptr<AfeAudioProcessor> afe_;
#endif
    AudioCodec* codec_ = nullptr;
    TaskHandle_t read_task_ = nullptr;
    volatile bool running_ = false;
};

}  // namespace eidolon
