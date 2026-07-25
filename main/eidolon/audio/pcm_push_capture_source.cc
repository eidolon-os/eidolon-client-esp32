#include "pcm_push_capture_source.h"

#include <esp_log.h>

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

    ring_ = xStreamBufferCreate(ring_capacity_bytes, /*trigger_level=*/1);
    if (ring_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u byte PCM ring",
                 static_cast<unsigned>(ring_capacity_bytes));
    }
}

PcmPushCaptureSource::~PcmPushCaptureSource() {
    if (ring_ != nullptr) {
        vStreamBufferDelete(ring_);
        ring_ = nullptr;
    }
}

PcmPushCaptureSource* PcmPushCaptureSource::From(esp_capture_audio_src_if_t* h) {
    return reinterpret_cast<PcmPushCaptureSource*>(h);
}

void PcmPushCaptureSource::Push(const int16_t* samples, size_t sample_count) {
    if (ring_ == nullptr || samples == nullptr || sample_count == 0) {
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
