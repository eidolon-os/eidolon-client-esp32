#include "aec_qualification.h"

#include "audio_codec.h"
#include "board.h"
#include "processors/afe_audio_processor.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/usb_serial_jtag.h>
#include <mbedtls/base64.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AecQualification"

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;
constexpr int kSilenceMs = 1600;
constexpr int kPlaybackMs = 4200;
constexpr int kBargeInMs = 6200;
constexpr int kManualPromptMs = 5000;
constexpr int kWarmupFrames = 10;
constexpr int kPcmPreviewFrames = 48;
constexpr int kPlaybackVolume = 75;
constexpr float kPi = 3.14159265358979323846f;

struct CaptureStats {
    uint32_t raw_frames = 0;
    uint32_t aec_frames = 0;
    uint32_t clipped_samples = 0;
    uint32_t dropout_frames = 0;
    uint64_t raw_energy = 0;
    uint64_t ref_energy = 0;
    uint64_t aec_energy = 0;
    uint64_t playback_energy = 0;
    int64_t feed_us = 0;
    int64_t max_feed_us = 0;
};

std::string Base64Encode(const int16_t* samples, size_t count)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    size_t byte_count = count * sizeof(int16_t);
    size_t out_len = 0;
    size_t cap = ((byte_count + 2) / 3) * 4 + 1;
    std::string out(cap, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &out_len,
                              bytes, byte_count) != 0) {
        return "";
    }
    out.resize(out_len);
    return out;
}

void EmitEvent(const char* type, const std::string& fields = "")
{
    std::string line = "AECQ {\"type\":\"";
    line += type;
    line += "\",\"ts_us\":";
    line += std::to_string(static_cast<long long>(esp_timer_get_time()));
    if (fields.empty()) {
        line += "}\n";
    } else {
        line += ",";
        line += fields;
        line += "}\n";
    }
    fputs(line.c_str(), stdout);
    fflush(stdout);
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(line.data(), line.size(), pdMS_TO_TICKS(20));
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(20));
#endif
}

void EmitAudioFrame(const char* case_id, const char* stream_id, uint32_t seq,
                    const int16_t* samples, size_t sample_count, int channels)
{
    std::string b64 = Base64Encode(samples, sample_count);
    std::string line = "AECQ {\"type\":\"audio\",\"case_id\":\"";
    line += case_id;
    line += "\",\"stream_id\":\"";
    line += stream_id;
    line += "\",\"seq\":";
    line += std::to_string(static_cast<unsigned long>(seq));
    line += ",\"ts_us\":";
    line += std::to_string(static_cast<long long>(esp_timer_get_time()));
    line += ",\"sample_rate\":";
    line += std::to_string(kSampleRate);
    line += ",\"channels\":";
    line += std::to_string(channels);
    line += ",\"encoding\":\"pcm_s16le_b64\",\"data\":\"";
    line += b64;
    line += "\"}\n";
    fputs(line.c_str(), stdout);
    fflush(stdout);
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(line.data(), line.size(), pdMS_TO_TICKS(20));
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(20));
#endif
}

int16_t ClampToI16(float sample)
{
    if (sample > 32767.0f) {
        return 32767;
    }
    if (sample < -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(sample);
}

void BuildPlaybackFrame(std::vector<int16_t>& frame, int frame_index, bool enabled)
{
    frame.resize(kFrameSamples);
    if (!enabled) {
        std::fill(frame.begin(), frame.end(), 0);
        return;
    }

    // Deterministic far-end signal: a speech-like multi-tone with soft envelope.
    const int sample_offset = frame_index * kFrameSamples;
    for (int i = 0; i < kFrameSamples; ++i) {
        float t = static_cast<float>(sample_offset + i) / static_cast<float>(kSampleRate);
        float tone = 0.48f * std::sin(2.0f * kPi * 420.0f * t) +
                     0.32f * std::sin(2.0f * kPi * 870.0f * t) +
                     0.20f * std::sin(2.0f * kPi * 1410.0f * t);
        float gate = 0.65f + 0.35f * std::sin(2.0f * kPi * 3.7f * t);
        frame[i] = ClampToI16(tone * gate * 10500.0f);
    }
}

uint64_t Energy(const int16_t* samples, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t s = samples[i];
        sum += static_cast<uint64_t>(s * s);
    }
    return sum;
}

uint32_t CountClipped(const int16_t* samples, size_t count)
{
    uint32_t clipped = 0;
    for (size_t i = 0; i < count; ++i) {
        if (samples[i] >= 32760 || samples[i] <= -32760) {
            ++clipped;
        }
    }
    return clipped;
}

std::vector<int16_t> ExtractChannel(const std::vector<int16_t>& interleaved, int channels, int channel)
{
    std::vector<int16_t> out;
    if (channels <= 0 || channel >= channels) {
        return out;
    }
    out.reserve(interleaved.size() / channels);
    for (size_t i = static_cast<size_t>(channel); i < interleaved.size(); i += channels) {
        out.push_back(interleaved[i]);
    }
    return out;
}

std::string StatsJson(const CaptureStats& stats)
{
    return "\"raw_frames\":" + std::to_string(stats.raw_frames) +
           ",\"aec_frames\":" + std::to_string(stats.aec_frames) +
           ",\"dropout_frames\":" + std::to_string(stats.dropout_frames) +
           ",\"clipped_samples\":" + std::to_string(stats.clipped_samples) +
           ",\"raw_energy\":" + std::to_string(stats.raw_energy) +
           ",\"ref_energy\":" + std::to_string(stats.ref_energy) +
           ",\"aec_energy\":" + std::to_string(stats.aec_energy) +
           ",\"playback_energy\":" + std::to_string(stats.playback_energy) +
           ",\"feed_us\":" + std::to_string(stats.feed_us) +
           ",\"max_feed_us\":" + std::to_string(stats.max_feed_us);
}

class AecQualificationRunner {
public:
    void Run()
    {
        auto& board = Board::GetInstance();
        codec_ = board.GetAudioCodec();
        if (codec_ == nullptr) {
            EmitEvent("error", "\"message\":\"audio_codec_unavailable\"");
            return;
        }

        codec_->Start();
        codec_->SetOutputVolume(kPlaybackVolume);
        codec_->EnableInput(true);
        codec_->EnableOutput(true);

        afe_.Initialize(codec_, kFrameMs, nullptr);
        afe_.OnOutput([this](std::vector<int16_t>&& data) {
            CaptureStats* stats = active_stats_;
            if (active_case_ == nullptr || stats == nullptr) {
                return;
            }
            stats->aec_energy += Energy(data.data(), data.size());
            stats->aec_frames++;
            if (stats->aec_frames <= kPcmPreviewFrames) {
                EmitAudioFrame(active_case_, "aec_out", stats->aec_frames, data.data(), data.size(), 1);
            }
        });
        afe_.Start();
        afe_.EnableDeviceAec(true);

        EmitEvent("ready", "\"board\":\"" BOARD_NAME "\",\"board_type\":\"" BOARD_TYPE
                  "\",\"input_channels\":" + std::to_string(codec_->input_channels()) +
                  ",\"input_reference\":" + std::string(codec_->input_reference() ? "true" : "false") +
                  ",\"input_sample_rate\":" + std::to_string(codec_->input_sample_rate()) +
                  ",\"output_sample_rate\":" + std::to_string(codec_->output_sample_rate()) +
                  ",\"free_heap\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_8BIT)) +
                  ",\"free_internal_heap\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

        RunCase("silence", kSilenceMs, false, false);
        RunCase("far_playback", kPlaybackMs, true, false);
        EmitEvent("manual_prompt", "\"message\":\"place_near_speech_source_and_start_playback\","
                  "\"countdown_ms\":" + std::to_string(kManualPromptMs));
        vTaskDelay(pdMS_TO_TICKS(kManualPromptMs));
        RunCase("barge_in", kBargeInMs, true, true);

        EmitEvent("done", "\"free_heap\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_8BIT)) +
                  ",\"free_internal_heap\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

        afe_.Stop();
        codec_->EnableOutput(false);
        codec_->EnableInput(false);
    }

private:
    AudioCodec* codec_ = nullptr;
    AfeAudioProcessor afe_;
    const char* active_case_ = nullptr;
    CaptureStats* active_stats_ = nullptr;

    void RunCase(const char* case_id, int duration_ms, bool playback, bool manual_near)
    {
        CaptureStats stats;
        active_case_ = case_id;
        active_stats_ = &stats;

        EmitEvent("case_start", "\"case_id\":\"" + std::string(case_id) +
                  "\",\"duration_ms\":" + std::to_string(duration_ms) +
                  ",\"playback\":" + std::string(playback ? "true" : "false") +
                  ",\"manual_near\":" + std::string(manual_near ? "true" : "false"));

        int frames = duration_ms / kFrameMs;
        int channels = codec_->input_channels();
        std::vector<int16_t> playback_frame;
        std::vector<int16_t> input(static_cast<size_t>(kFrameSamples) * channels);

        for (int frame = 0; frame < frames; ++frame) {
            BuildPlaybackFrame(playback_frame, frame, playback);
            if (playback) {
                codec_->OutputData(playback_frame);
                stats.playback_energy += Energy(playback_frame.data(), playback_frame.size());
                if (frame < kPcmPreviewFrames) {
                    EmitAudioFrame(case_id, "playback_marker", frame + 1, playback_frame.data(),
                                   playback_frame.size(), 1);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(kFrameMs));
            }

            if (!codec_->InputData(input)) {
                stats.dropout_frames++;
                continue;
            }

            auto mic = ExtractChannel(input, channels, 0);
            auto ref = ExtractChannel(input, channels, channels > 1 ? channels - 1 : 0);
            stats.raw_energy += Energy(mic.data(), mic.size());
            stats.ref_energy += Energy(ref.data(), ref.size());
            stats.clipped_samples += CountClipped(mic.data(), mic.size());
            stats.raw_frames++;

            if (frame >= kWarmupFrames && frame < kPcmPreviewFrames + kWarmupFrames) {
                EmitAudioFrame(case_id, "raw_mic", frame + 1, mic.data(), mic.size(), 1);
                if (channels > 1) {
                    EmitAudioFrame(case_id, "reference", frame + 1, ref.data(), ref.size(), 1);
                }
            }

            int64_t start = esp_timer_get_time();
            afe_.Feed(std::move(input));
            int64_t elapsed = esp_timer_get_time() - start;
            stats.feed_us += elapsed;
            if (elapsed > stats.max_feed_us) {
                stats.max_feed_us = elapsed;
            }
            input.resize(static_cast<size_t>(kFrameSamples) * channels);
        }

        vTaskDelay(pdMS_TO_TICKS(160));
        EmitEvent("case_end", "\"case_id\":\"" + std::string(case_id) + "\"," + StatsJson(stats));
        active_stats_ = nullptr;
        active_case_ = nullptr;
    }
};
}  // namespace

void RunAecQualification()
{
    usb_serial_jtag_driver_config_t usb_serial_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usb_serial_ret = usb_serial_jtag_driver_install(&usb_serial_config);
    if (usb_serial_ret != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(usb_serial_ret));
    }

    ESP_LOGI(TAG, "Starting standalone AEC qualification runner");
    AecQualificationRunner runner;
    runner.Run();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
