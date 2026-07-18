#include "guard/person_presence_engine.h"

#include <algorithm>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "guard/guard_motion.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern const unsigned char g_person_detect_model_data[];
extern const int g_person_detect_model_data_len;

namespace eidolon {
namespace {

constexpr char kTag[] = "PersonPresence";
constexpr size_t kInputWidth = 96;
constexpr size_t kInputHeight = 96;
constexpr size_t kInputPixels = kInputWidth * kInputHeight;
constexpr int kPersonOutputIndex = 1;
constexpr int kNoPersonOutputIndex = 0;
constexpr float kPersonThreshold = 0.60f;
#if CONFIG_NN_OPTIMIZED
constexpr size_t kTensorArenaSize = 160 * 1024;
#else
constexpr size_t kTensorArenaSize = 100 * 1024;
#endif

bool HasExpectedInput(const TfLiteTensor* tensor)
{
    return tensor != nullptr && tensor->type == kTfLiteInt8 &&
           tensor->bytes >= kInputPixels && tensor->dims != nullptr &&
           tensor->dims->size == 4 && tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == static_cast<int>(kInputHeight) &&
           tensor->dims->data[2] == static_cast<int>(kInputWidth) &&
           tensor->dims->data[3] == 1;
}

float Dequantize(int8_t value, const TfLiteTensor* tensor)
{
    return (static_cast<int>(value) - tensor->params.zero_point) *
           tensor->params.scale;
}

}  // namespace

class PersonPresenceEngine::Impl {
public:
    Impl()
    {
        model_ = tflite::GetModel(g_person_detect_model_data);
        if (g_person_detect_model_data_len <= 0 || model_ == nullptr ||
            model_->version() != TFLITE_SCHEMA_VERSION) {
            ESP_LOGE(kTag, "Invalid person detection model");
            return;
        }
        if (resolver_.AddAveragePool2D() != kTfLiteOk ||
            resolver_.AddConv2D() != kTfLiteOk ||
            resolver_.AddDepthwiseConv2D() != kTfLiteOk ||
            resolver_.AddReshape() != kTfLiteOk ||
            resolver_.AddSoftmax() != kTfLiteOk) {
            ESP_LOGE(kTag, "Failed to register person detection operators");
            return;
        }
        arena_ = static_cast<uint8_t*>(heap_caps_malloc(
            kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (arena_ == nullptr) {
            ESP_LOGE(kTag, "Failed to allocate %u-byte PSRAM tensor arena",
                     static_cast<unsigned>(kTensorArenaSize));
            return;
        }
        interpreter_ = new (std::nothrow) tflite::MicroInterpreter(
            model_, resolver_, arena_, kTensorArenaSize);
        if (interpreter_ == nullptr || interpreter_->AllocateTensors() != kTfLiteOk) {
            ESP_LOGE(kTag, "Failed to allocate person detection tensors");
            return;
        }
        input_ = interpreter_->input(0);
        output_ = interpreter_->output(0);
        if (!HasExpectedInput(input_) || output_ == nullptr ||
            output_->type != kTfLiteInt8 || output_->bytes < 2) {
            ESP_LOGE(kTag, "Unexpected person detection tensor contract");
            return;
        }
        ready_ = true;
        ESP_LOGI(kTag,
                 "Ready model_bytes=%d arena_bytes=%u internal_free=%u psram_free=%u",
                 g_person_detect_model_data_len,
                 static_cast<unsigned>(kTensorArenaSize),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    ~Impl()
    {
        delete interpreter_;
        if (arena_ != nullptr) {
            heap_caps_free(arena_);
        }
    }

    PersonPresenceResult AnalyzeFrame(const CameraFrame& frame, uint64_t now_ms)
    {
        PersonPresenceResult result;
        if (!ready_ || now_ms < next_inference_ms_) {
            return result;
        }
        next_inference_ms_ = now_ms + interval_ms_;
        auto* input_bytes = reinterpret_cast<uint8_t*>(input_->data.int8);
        if (!ReadGuardLuminanceImage(frame, input_bytes, kInputWidth,
                                     kInputHeight, true)) {
            return result;
        }
        for (size_t index = 0; index < kInputPixels; ++index) {
            input_bytes[index] ^= 0x80;
        }
        const int64_t started_us = esp_timer_get_time();
        if (interpreter_->Invoke() != kTfLiteOk) {
            ESP_LOGE(kTag, "Person detection inference failed");
            return result;
        }
        result.inference_us = static_cast<uint64_t>(esp_timer_get_time() - started_us);
        result.person_score = Dequantize(output_->data.int8[kPersonOutputIndex], output_);
        result.no_person_score =
            Dequantize(output_->data.int8[kNoPersonOutputIndex], output_);
        result.present = result.person_score >= kPersonThreshold &&
                         result.person_score > result.no_person_score;
        result.evaluated = true;
        result.evaluated_at_ms = now_ms;
        return result;
    }

    void SetIntervalMs(uint32_t interval_ms)
    {
        interval_ms_ = std::max<uint32_t>(interval_ms, 200);
    }

    bool ready() const { return ready_; }

private:
    const tflite::Model* model_ = nullptr;
    tflite::MicroMutableOpResolver<5> resolver_;
    uint8_t* arena_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    uint32_t interval_ms_ = 500;
    uint64_t next_inference_ms_ = 0;
    bool ready_ = false;
};

PersonPresenceEngine::PersonPresenceEngine()
    : impl_(std::make_unique<Impl>())
{
}

PersonPresenceEngine::~PersonPresenceEngine() = default;

PersonPresenceResult PersonPresenceEngine::AnalyzeFrame(const CameraFrame& frame,
                                                         uint64_t now_ms)
{
    return impl_ != nullptr ? impl_->AnalyzeFrame(frame, now_ms)
                            : PersonPresenceResult{};
}

void PersonPresenceEngine::SetIntervalMs(uint32_t interval_ms)
{
    if (impl_ != nullptr) {
        impl_->SetIntervalMs(interval_ms);
    }
}

bool PersonPresenceEngine::ready() const
{
    return impl_ != nullptr && impl_->ready();
}

}  // namespace eidolon
