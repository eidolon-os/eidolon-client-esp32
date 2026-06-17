#include "eidolon_afe_capture.h"

#include "audio_codec.h"
#include "livekit_board.h"
#include "processors/afe_audio_processor.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cmath>
#include <utility>
#include <vector>

#if CONFIG_USE_AUDIO_DEBUGGER
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#endif

#define TAG "EidolonAfeCap"

namespace {
// AFE output framing. The PcmPushCaptureSource ring decouples this from the
// encoder pull size, so the exact value only affects callback granularity.
constexpr int kAfeFrameDurationMs = 20;
// 10 ms mic read chunk @ 16 kHz, matching the xiaozhi AudioInputTask cadence.
constexpr int kReadFramesPerChannel = 160;
// The read loop also drives AfeAudioProcessor::Feed (ESP-SR AFE feed in the
// VC/HIGH_PERF profile), which is stack-heavy; 4 KB overflows. xiaozhi's
// equivalent input task uses ~6 KB — give headroom.
constexpr int kReadTaskStack = 8192;
// Discard the first ~150 ms of mic after start so the codec DMA / AEC settle
// before we feed the AFE (mirrors xiaozhi's audio-input warmup).
constexpr int kWarmupFrames = 15;
// How long a near-end speech edge keeps NearEndActiveRecently() true after the
// energy drops below threshold. Bridges the small gaps between words so a single
// barge-in utterance reads as one continuous "user is talking" window.
constexpr int64_t kNearEndHangoverUs = 700 * 1000;
// Energy-gate parameters for near-end detection on the AEC-cleaned output.
// Threshold sits well above the measured residual-echo floor (RMS ~20-50) and
// below real speech (~200+). Persistence rejects lone residual spikes.
constexpr int kRmsWindowSamples = 1600;       // ~100 ms @ 16 kHz
constexpr double kNearEndRmsThreshold = 120.0;
constexpr int kNearEndMinWindows = 2;         // ~200 ms sustained
// Suppress near-end detection for this long after the AFE starts. The AEC3
// adaptive filter has not converged yet, so the first playback (the welcome
// message) leaks strong, un-cancelled echo that would otherwise trip the gate
// and cut off / pollute the welcome. During this window near_end stays false,
// so mic_muted stays true and the channel drops the (echo) audio — i.e. the
// device is gracefully half-duplex until the AEC settles, with no separate
// "hard mute" state.
constexpr int64_t kNearEndStartupSuppressUs = 2000 * 1000;

#if CONFIG_USE_AUDIO_DEBUGGER
// Echo-quantification taps (diagnostic). Streams three raw 16 kHz mono PCM
// channels over UDP so they can be compared on a PC:
//   mic ch0 (pre-AEC)        -> CONFIG_AUDIO_DEBUG_UDP_SERVER port
//   AFE out (post-AEC)       -> that port + 1
//   reference ch1 (loopback) -> that port + 2   (the echo reference fed to AEC)
int s_dump_mic_sock = -1;
int s_dump_out_sock = -1;
int s_dump_ref_sock = -1;
sockaddr_in s_dump_mic_addr{};
sockaddr_in s_dump_out_addr{};
sockaddr_in s_dump_ref_addr{};
bool s_dump_open = false;

void DumpOpen() {
    if (s_dump_open) {
        return;
    }
    const char* cfg = CONFIG_AUDIO_DEBUG_UDP_SERVER;  // "IP:PORT"
    char ip[40] = {0};
    int port = 8000;
    const char* colon = strchr(cfg, ':');
    if (colon) {
        size_t n = static_cast<size_t>(colon - cfg);
        if (n < sizeof(ip)) {
            memcpy(ip, cfg, n);
        }
        port = atoi(colon + 1);
    } else {
        strncpy(ip, cfg, sizeof(ip) - 1);
    }
    s_dump_mic_sock = socket(AF_INET, SOCK_DGRAM, 0);
    s_dump_out_sock = socket(AF_INET, SOCK_DGRAM, 0);
    s_dump_ref_sock = socket(AF_INET, SOCK_DGRAM, 0);
    s_dump_mic_addr.sin_family = AF_INET;
    s_dump_mic_addr.sin_port = htons(port);
    s_dump_mic_addr.sin_addr.s_addr = inet_addr(ip);
    s_dump_out_addr.sin_family = AF_INET;
    s_dump_out_addr.sin_port = htons(port + 1);
    s_dump_out_addr.sin_addr.s_addr = inet_addr(ip);
    s_dump_ref_addr.sin_family = AF_INET;
    s_dump_ref_addr.sin_port = htons(port + 2);
    s_dump_ref_addr.sin_addr.s_addr = inet_addr(ip);
    s_dump_open = true;
}

void DumpSend(int sock, const sockaddr_in& addr, const int16_t* d, size_t n) {
    if (sock >= 0 && d && n) {
        sendto(sock, d, n * sizeof(int16_t), 0, reinterpret_cast<const sockaddr*>(&addr),
               sizeof(addr));
    }
}
#endif  // CONFIG_USE_AUDIO_DEBUGGER
}  // namespace

namespace eidolon {

EidolonAfeCapture::EidolonAfeCapture() = default;

EidolonAfeCapture::~EidolonAfeCapture() {
    Stop();
}

esp_err_t EidolonAfeCapture::Start(AudioCodec* codec) {
    if (running_) {
        return ESP_OK;
    }
    if (codec == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    codec_ = codec;
    codec_->Start();
    codec_->EnableInput(true);

    // Fresh AFE per session: avoids carrying stale internal state across
    // start/stop (which could fault in afe_feed/afe_parse_input). Safe now that
    // AfeAudioProcessor's destructor stops and joins its internal task before
    // freeing afe_data_.
    afe_ = std::make_unique<AfeAudioProcessor>();
    // nullptr models -> AfeAudioProcessor loads the on-flash "model" partition.
    afe_->Initialize(codec_, kAfeFrameDurationMs, nullptr);
    afe_->OnOutput([this](std::vector<int16_t>&& data) {
#if CONFIG_USE_AUDIO_DEBUGGER
        DumpSend(s_dump_out_sock, s_dump_out_addr, data.data(), data.size());
#endif
        // Energy-based near-end detection on the AEC-cleaned output. On-device
        // measurement showed the residual-echo floor sits at RMS ~20-50 while real
        // near-end speech is ~200-867 (≈20 dB apart), so an energy gate separates
        // them robustly — unlike the AFE VAD, which fired on the noise floor.
        for (int16_t s : data) {
            rms_sumsq_ += static_cast<double>(s) * static_cast<double>(s);
        }
        rms_count_ += data.size();
        if (rms_count_ >= kRmsWindowSamples) {  // ~100 ms @ 16 kHz
            double rms = sqrt(rms_sumsq_ / rms_count_);
            int64_t now = esp_timer_get_time();
            bool in_startup = (now - afe_start_us_) < kNearEndStartupSuppressUs;
            if (rms > kNearEndRmsThreshold && !in_startup) {
                // Require persistence so a lone residual-echo spike can't trip it;
                // sustained speech crosses the threshold for many windows.
                if (++near_end_run_ >= kNearEndMinWindows) {
                    last_near_end_us_ = now;
                }
            } else {
                near_end_run_ = 0;
            }
            if (++rms_log_div_ >= 5) {  // ~500 ms cadence, readable on serial
                ESP_LOGI(TAG, "out RMS=%.0f near_end=%d", rms,
                         NearEndActiveRecently() ? 1 : 0);
                rms_log_div_ = 0;
            }
            rms_sumsq_ = 0.0;
            rms_count_ = 0;
        }
        pcm_source_.Push(data.data(), data.size());
    });
    // Near-end (real user) speech detected by the AFE on the AEC-cleaned signal.
    // Drives full-duplex barge-in: echo is removed before the VAD sees it, so a
    // speech edge here means the user is genuinely talking over the agent.
    afe_->OnVadStateChange([this](bool speaking) { OnVadState(speaking); });

#if CONFIG_USE_AUDIO_DEBUGGER
    DumpOpen();
#endif

    running_ = true;
    // Mark the AEC start for the near-end startup-suppression window (AEC3 needs
    // a couple seconds to converge; the welcome message plays during that window).
    afe_start_us_ = esp_timer_get_time();
    afe_->Start();
    // Enable AEC but keep VAD running (unlike EnableDeviceAec(true), which would
    // disable VAD) so we retain the near-end voice-activity signal for barge-in.
    afe_->EnableAecKeepVad();

    // Pinned to core 0 at priority 8, matching xiaozhi's audio-input task.
    BaseType_t created = xTaskCreatePinnedToCore([](void* arg) {
        static_cast<EidolonAfeCapture*>(arg)->ReadLoop();
        vTaskDelete(nullptr);
    }, "eidolon_afe_rd", kReadTaskStack, this, 8, &read_task_, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE read task");
        running_ = false;
        afe_.reset();  // safe: joins the internal task
        read_task_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE capture started (channels=%d, %d Hz)", codec_->input_channels(),
             codec_->input_sample_rate());
    return ESP_OK;
}

void EidolonAfeCapture::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    // The read loop polls running_ and clears read_task_ before exiting. Stop the
    // reader first so no Feed() runs concurrently with afe_->Stop().
    for (int i = 0; i < 100 && read_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Destroy the AFE for this session. Safe: ~AfeAudioProcessor stops and joins
    // its internal fetch task before freeing afe_data_.
    if (afe_) {
        afe_->Stop();
        afe_.reset();
    }
    ESP_LOGI(TAG, "AFE capture stopped");
}

void EidolonAfeCapture::ReadLoop() {
    const int channels = codec_->input_channels();
    int warmup = kWarmupFrames;
    std::vector<int16_t> data;
    while (running_) {
        data.resize(static_cast<size_t>(kReadFramesPerChannel) * channels);
        if (!codec_->InputData(data)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (warmup > 0) {
            --warmup;  // drain but don't feed yet (let codec/AEC settle)
            continue;
        }
        // Software AEC reference: this board's hardware reference (ES7210 MIC3) is
        // crosstalk-contaminated, so overwrite the dead ch1 (unpopulated MIC2) with
        // the PCM that was sent to the speaker. The AFE AEC (input_format "MR") then
        // cancels echo against a clean reference; its delay estimator absorbs the
        // small DAC+acoustic offset. Idle playback yields silence (no echo).
        if (channels >= 2) {
            int16_t ref[kReadFramesPerChannel];
            eidolon_livekit_board_pull_playback_reference(ref, kReadFramesPerChannel);
            for (int i = 0; i < kReadFramesPerChannel; ++i) {
                data[static_cast<size_t>(i) * channels + 1] = ref[i];
            }
        }
#if CONFIG_USE_AUDIO_DEBUGGER
        {
            static std::vector<int16_t> mono;
            mono.resize(kReadFramesPerChannel);
            // Pre-AEC mic (channel 0).
            for (int i = 0; i < kReadFramesPerChannel; ++i) {
                mono[i] = data[static_cast<size_t>(i) * channels];
            }
            DumpSend(s_dump_mic_sock, s_dump_mic_addr, mono.data(), mono.size());
            // Echo reference (channel 1) — the speaker loopback fed to the AEC.
            if (channels >= 2) {
                for (int i = 0; i < kReadFramesPerChannel; ++i) {
                    mono[i] = data[static_cast<size_t>(i) * channels + 1];
                }
                DumpSend(s_dump_ref_sock, s_dump_ref_addr, mono.data(), mono.size());
            }
        }
#endif
        afe_->Feed(std::move(data));
    }
    read_task_ = nullptr;
}

void EidolonAfeCapture::OnVadState(bool speaking) {
    // The AFE VAD proved unreliable for near-end detection (it fires on the
    // residual-echo noise floor), so it no longer drives the barge-in signal —
    // the energy gate in OnOutput does. Kept only as a diagnostic edge log to
    // compare VAD vs energy while tuning.
    ESP_LOGD(TAG, "AFE VAD -> %s (diagnostic only)", speaking ? "SPEECH" : "silence");
}

bool EidolonAfeCapture::NearEndActiveRecently() const {
    int64_t last = last_near_end_us_;
    if (last == 0) {
        return false;
    }
    return (esp_timer_get_time() - last) < kNearEndHangoverUs;
}

}  // namespace eidolon
