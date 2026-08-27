#include "pcm_push_capture_source.h"

#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include <cstring>

#define TAG "PcmPushSrc"

namespace eidolon {

namespace {
constexpr TickType_t kReadChunkTimeout = pdMS_TO_TICKS(200);
}  // namespace

PcmPushCaptureSource::PcmPushCaptureSource(uint32_t sample_rate, size_t ring_capacity_bytes)
    : sample_rate_(sample_rate) {
    base_.open = Open;
    base_.get_support_codecs = GetSupportCodecs;
    base_.negotiate_caps = NegotiateCaps;
    base_.start = Start;
    base_.read_frame = ReadFrame;
    base_.stop = Stop;
    base_.close = Close;

#if CONFIG_BOARD_TYPE_ESP_BOX_3
    // BOX-3 full-duplex AEC has been hardware-qualified with the PCM ring in
    // internal SRAM. Putting this real-time producer/consumer path in PSRAM
    // causes corrupted/noisy capture under concurrent codec, display and Wi-Fi
    // load, which in turn prevents STT from producing a usable utterance.
    ring_ = xStreamBufferCreate(ring_capacity_bytes, /*trigger_level=*/1);
#else
    // Internal-SRAM-tight boards keep the ring in PSRAM so the LiveKit engine
    // can still obtain its internal-only event queue and task stack.
    ring_ = xStreamBufferCreateWithCaps(ring_capacity_bytes, /*trigger_level=*/1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (ring_ == nullptr) {
#if CONFIG_BOARD_TYPE_ESP_BOX_3
        ESP_LOGE(TAG, "Failed to allocate %u byte PCM ring in internal SRAM (internal_free=%u)",
                 static_cast<unsigned>(ring_capacity_bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#else
        ESP_LOGE(TAG, "Failed to allocate %u byte PCM ring in PSRAM (psram_free=%u)",
                 static_cast<unsigned>(ring_capacity_bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#endif
    }
}

PcmPushCaptureSource::~PcmPushCaptureSource() {
    if (ring_ != nullptr) {
#if CONFIG_BOARD_TYPE_ESP_BOX_3
        vStreamBufferDelete(ring_);
#else
        vStreamBufferDeleteWithCaps(ring_);
#endif
        ring_ = nullptr;
    }
}

PcmPushCaptureSource* PcmPushCaptureSource::From(esp_capture_audio_src_if_t* h) {
    return reinterpret_cast<PcmPushCaptureSource*>(h);
}

void PcmPushCaptureSource::Push(const int16_t* samples, size_t sample_count) {
    // The mic/AFE is initialized before LiveKit starts the esp_capture source.
    // Audio produced before Start() has no consumer and Start() flushes it
    // anyway, so do not fill the ring during room negotiation.
    if (!running_ || ring_ == nullptr || samples == nullptr || sample_count == 0) {
        return;
    }
    const size_t bytes = sample_count * sizeof(int16_t);
    size_t sent = xStreamBufferSend(ring_, samples, bytes, /*ticks_to_wait=*/0);
    if (sent < bytes) {
        // Consumer is behind; drop the overflow rather than block the AFE task.
        static uint32_t dropped_logs = 0;
        if ((dropped_logs++ & 0x3f) == 0) {
            ESP_LOGW(TAG, "PCM ring full, dropped %u bytes", static_cast<unsigned>(bytes - sent));
        }
    }
}

void PcmPushCaptureSource::Flush() {
    if (ring_ != nullptr) {
        xStreamBufferReset(ring_);
    }
}

void PcmPushCaptureSource::Quiesce() {
    // Clear running_ first so ReadFrame's loop (while got<need && running_) will
    // exit, then wake a reader currently blocked in xStreamBufferReceive with a
    // 1-sample sentinel so it returns NOW instead of after the 200 ms chunk
    // timeout. The reader unblocks, sees running_==false, and returns; esp_capture's
    // fetch thread then exits before the pipeline is torn down.
    running_ = false;
    if (ring_ != nullptr) {
        const int16_t sentinel = 0;
        xStreamBufferSend(ring_, &sentinel, sizeof(sentinel), /*ticks_to_wait=*/0);
    }
}

esp_capture_err_t PcmPushCaptureSource::Open(esp_capture_audio_src_if_t* h) {
    return From(h)->ring_ != nullptr ? ESP_CAPTURE_ERR_OK : ESP_CAPTURE_ERR_NO_MEM;
}

esp_capture_err_t PcmPushCaptureSource::GetSupportCodecs(esp_capture_audio_src_if_t* /*h*/,
                                                         const esp_capture_format_id_t** codecs,
                                                         uint8_t* num) {
    static esp_capture_format_id_t kSupported[] = {ESP_CAPTURE_FMT_ID_PCM};
    *codecs = kSupported;
    *num = 1;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t PcmPushCaptureSource::NegotiateCaps(esp_capture_audio_src_if_t* h,
                                                      esp_capture_audio_info_t* in_caps,
                                                      esp_capture_audio_info_t* out_caps) {
    if (in_caps->format_id != ESP_CAPTURE_FMT_ID_PCM) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    out_caps->format_id = ESP_CAPTURE_FMT_ID_PCM;
    out_caps->sample_rate = From(h)->sample_rate_;
    out_caps->channel = 1;
    out_caps->bits_per_sample = 16;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t PcmPushCaptureSource::Start(esp_capture_audio_src_if_t* h) {
    auto* self = From(h);
    self->Flush();
    self->samples_read_ = 0;
    self->running_ = true;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t PcmPushCaptureSource::ReadFrame(esp_capture_audio_src_if_t* h,
                                                  esp_capture_stream_frame_t* frame) {
    auto* self = From(h);
    if (!self->running_ || self->ring_ == nullptr) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }

    frame->pts = static_cast<uint32_t>(self->samples_read_ * 1000 / self->sample_rate_);

    size_t need = static_cast<size_t>(frame->size);
    size_t got = 0;
    while (got < need && self->running_) {
        size_t r = xStreamBufferReceive(self->ring_, frame->data + got, need - got, kReadChunkTimeout);
        got += r;
        if (r == 0 && !self->running_) {
            break;
        }
    }

    frame->size = static_cast<int>(got);
    self->samples_read_ += got / sizeof(int16_t);
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t PcmPushCaptureSource::Stop(esp_capture_audio_src_if_t* h) {
    From(h)->running_ = false;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t PcmPushCaptureSource::Close(esp_capture_audio_src_if_t* /*h*/) {
    return ESP_CAPTURE_ERR_OK;
}

}  // namespace eidolon
