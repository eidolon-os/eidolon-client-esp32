// microWakeWord inference for Eidolon Hub (Apache-2.0, derived from ESPHome micro_wake_word).

#include "eidolon_mww.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/schema/schema_generated.h"

#define TAG "EidolonMww"

extern const uint8_t hey_jarvis_tflite_start[] asm("_binary_hey_jarvis_tflite_start");
extern const uint8_t hey_jarvis_tflite_end[] asm("_binary_hey_jarvis_tflite_end");

namespace {

constexpr uint8_t kPreprocessorFeatureSize = 40;
constexpr uint8_t kFeatureDurationMs = 30;
constexpr uint8_t kFeaturesStepMs = 10;
constexpr uint8_t kMinSlicesBeforeDetection = 100;
constexpr uint32_t kVariableArenaSize = 1024;
constexpr size_t kDefaultTensorArenaSize = 80000;

constexpr float kFilterbankLower = 125.0f;
constexpr float kFilterbankUpper = 7500.0f;

static FrontendConfig s_frontend_config;
static FrontendState s_frontend_state;
static bool s_frontend_ready = false;

struct StreamingDetector {
    const uint8_t* model_start = nullptr;
    size_t model_size = 0;
    uint8_t probability_cutoff = 128;
    uint8_t sliding_window_size = 10;
    int ignore_windows = -static_cast<int>(kMinSlicesBeforeDetection);

    uint8_t* var_arena = nullptr;
    uint8_t* tensor_arena = nullptr;
    size_t tensor_arena_size = kDefaultTensorArenaSize;
    tflite::MicroAllocator* allocator = nullptr;
    tflite::MicroResourceVariables* resource_vars = nullptr;
    std::unique_ptr<tflite::MicroInterpreter> interpreter;
    tflite::MicroMutableOpResolver<20> op_resolver;

    std::vector<uint8_t> recent_probs;
    size_t last_n_index = 0;
    uint8_t current_stride_step = 0;
    bool loaded = false;
    bool unprocessed = false;

    bool RegisterOps();
    bool Load();
    void Unload();
    bool Infer(const int8_t features[kPreprocessorFeatureSize]);
    bool CheckDetected();
};

static StreamingDetector s_detector;
static bool s_initialized = false;
static int64_t s_last_detect_us = 0;
static uint32_t s_cooldown_ms = 2000;
static bool s_pending_detection = false;

static uint8_t* AllocPsram(size_t size)
{
    return static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void FreePsram(uint8_t* ptr, size_t size)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
    (void)size;
}

static int8_t QuantizeFeature(int16_t value)
{
    constexpr int32_t value_scale = 256;
    constexpr int32_t value_div = 666;
    int32_t scaled = ((static_cast<int32_t>(value) * value_scale) + (value_div / 2)) / value_div;
    scaled += INT8_MIN;
    if (scaled < INT8_MIN) {
        return INT8_MIN;
    }
    if (scaled > INT8_MAX) {
        return INT8_MAX;
    }
    return static_cast<int8_t>(scaled);
}

static bool InitFrontend(int sample_rate_hz)
{
    if (s_frontend_ready) {
        return true;
    }

    s_frontend_config.window.size_ms = kFeatureDurationMs;
    s_frontend_config.window.step_size_ms = kFeaturesStepMs;
    s_frontend_config.filterbank.num_channels = kPreprocessorFeatureSize;
    s_frontend_config.filterbank.lower_band_limit = kFilterbankLower;
    s_frontend_config.filterbank.upper_band_limit = kFilterbankUpper;
    s_frontend_config.noise_reduction.smoothing_bits = 10;
    s_frontend_config.noise_reduction.even_smoothing = 0.025f;
    s_frontend_config.noise_reduction.odd_smoothing = 0.06f;
    s_frontend_config.noise_reduction.min_signal_remaining = 0.05f;
    s_frontend_config.pcan_gain_control.enable_pcan = true;
    s_frontend_config.pcan_gain_control.strength = 0.95f;
    s_frontend_config.pcan_gain_control.offset = 80.0f;
    s_frontend_config.pcan_gain_control.gain_bits = 21;
    s_frontend_config.log_scale.enable_log = true;
    s_frontend_config.log_scale.scale_shift = 6;

    if (!FrontendPopulateState(&s_frontend_config, &s_frontend_state, sample_rate_hz)) {
        ESP_LOGE(TAG, "FrontendPopulateState failed");
        return false;
    }
    s_frontend_ready = true;
    return true;
}

static size_t GenerateFeatures(int16_t* audio, size_t samples_available, int8_t* out_features)
{
    size_t processed_samples = 0;
    FrontendOutput output =
        FrontendProcessSamples(&s_frontend_state, audio, samples_available, &processed_samples);
    for (size_t i = 0; i < output.size; ++i) {
        out_features[i] = QuantizeFeature(output.values[i]);
    }
    return processed_samples;
}

bool StreamingDetector::RegisterOps()
{
    if (op_resolver.AddCallOnce() != kTfLiteOk) return false;
    if (op_resolver.AddVarHandle() != kTfLiteOk) return false;
    if (op_resolver.AddReshape() != kTfLiteOk) return false;
    if (op_resolver.AddReadVariable() != kTfLiteOk) return false;
    if (op_resolver.AddStridedSlice() != kTfLiteOk) return false;
    if (op_resolver.AddConcatenation() != kTfLiteOk) return false;
    if (op_resolver.AddAssignVariable() != kTfLiteOk) return false;
    if (op_resolver.AddConv2D() != kTfLiteOk) return false;
    if (op_resolver.AddMul() != kTfLiteOk) return false;
    if (op_resolver.AddAdd() != kTfLiteOk) return false;
    if (op_resolver.AddMean() != kTfLiteOk) return false;
    if (op_resolver.AddFullyConnected() != kTfLiteOk) return false;
    if (op_resolver.AddLogistic() != kTfLiteOk) return false;
    if (op_resolver.AddQuantize() != kTfLiteOk) return false;
    if (op_resolver.AddDepthwiseConv2D() != kTfLiteOk) return false;
    if (op_resolver.AddAveragePool2D() != kTfLiteOk) return false;
    if (op_resolver.AddMaxPool2D() != kTfLiteOk) return false;
    if (op_resolver.AddPad() != kTfLiteOk) return false;
    if (op_resolver.AddPack() != kTfLiteOk) return false;
    if (op_resolver.AddSplitV() != kTfLiteOk) return false;
    return true;
}

bool StreamingDetector::Load()
{
    if (loaded) {
        return true;
    }

    if (!RegisterOps()) {
        ESP_LOGE(TAG, "Failed to register TFLite ops");
        return false;
    }

    if (var_arena == nullptr) {
        var_arena = AllocPsram(kVariableArenaSize);
        if (!var_arena) {
            ESP_LOGE(TAG, "var arena alloc failed");
            return false;
        }
        allocator = tflite::MicroAllocator::Create(var_arena, kVariableArenaSize);
        resource_vars = tflite::MicroResourceVariables::Create(allocator, 20);
    }

    const tflite::Model* model = tflite::GetModel(model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Unsupported model schema");
        return false;
    }

    if (tensor_arena == nullptr) {
        tensor_arena = AllocPsram(tensor_arena_size);
        if (!tensor_arena) {
            ESP_LOGE(TAG, "tensor arena alloc failed");
            return false;
        }
    }

    interpreter = std::make_unique<tflite::MicroInterpreter>(
        model, op_resolver, tensor_arena, tensor_arena_size, resource_vars);
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        interpreter.reset();
        return false;
    }

    TfLiteTensor* input = interpreter->input(0);
    if (input->dims->size != 3 || input->dims->data[0] != 1 ||
        input->dims->data[2] != kPreprocessorFeatureSize) {
        ESP_LOGE(TAG, "Unexpected input tensor shape");
        return false;
    }
    if (input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "Expected int8 input");
        return false;
    }

    TfLiteTensor* output = interpreter->output(0);
    if (output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1) {
        ESP_LOGE(TAG, "Unexpected output tensor shape");
        return false;
    }
    if (output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Expected uint8 output");
        return false;
    }

    loaded = true;
    current_stride_step = 0;
    last_n_index = 0;
    std::fill(recent_probs.begin(), recent_probs.end(), 0);
    ignore_windows = -static_cast<int>(kMinSlicesBeforeDetection);
    ESP_LOGI(TAG, "hey_jarvis model loaded, arena used %zu bytes",
             interpreter->arena_used_bytes());
    return true;
}

void StreamingDetector::Unload()
{
    interpreter.reset();
    if (tensor_arena) {
        FreePsram(tensor_arena, tensor_arena_size);
        tensor_arena = nullptr;
    }
    if (var_arena) {
        FreePsram(var_arena, kVariableArenaSize);
        var_arena = nullptr;
        allocator = nullptr;
        resource_vars = nullptr;
    }
    loaded = false;
}

bool StreamingDetector::Infer(const int8_t features[kPreprocessorFeatureSize])
{
    if (!loaded && !Load()) {
        return false;
    }

    TfLiteTensor* input = interpreter->input(0);
    uint8_t stride = input->dims->data[1];
    current_stride_step = current_stride_step % stride;

    int8_t* input_data = tflite::GetTensorData<int8_t>(input);
    std::memmove(input_data + kPreprocessorFeatureSize * current_stride_step, features,
                 kPreprocessorFeatureSize);
    ++current_stride_step;

    if (current_stride_step < stride) {
        return true;
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "Invoke failed");
        return false;
    }

    uint8_t prob = interpreter->output(0)->data.uint8[0];
    last_n_index = (last_n_index + 1) % sliding_window_size;
    recent_probs[last_n_index] = prob;
    unprocessed = true;

    if (recent_probs[last_n_index] < probability_cutoff) {
        ignore_windows = std::min(ignore_windows + 1, 0);
    }
    return true;
}

bool StreamingDetector::CheckDetected()
{
    if (!unprocessed) {
        return false;
    }
    unprocessed = false;

    if (ignore_windows < 0) {
        return false;
    }

    uint32_t sum = 0;
    uint8_t max_prob = 0;
    for (auto p : recent_probs) {
        max_prob = std::max(max_prob, p);
        sum += p;
    }
    uint8_t avg = static_cast<uint8_t>(sum / sliding_window_size);
    bool detected = sum > static_cast<uint32_t>(probability_cutoff) * sliding_window_size;
    if (detected) {
        ESP_LOGI(TAG, "Wake score avg=%u max=%u (cutoff=%u)", avg, max_prob, probability_cutoff);
        ignore_windows = -static_cast<int>(kMinSlicesBeforeDetection);
        std::fill(recent_probs.begin(), recent_probs.end(), 0);
    }
    return detected;
}

}  // namespace

extern "C" esp_err_t eidolon_mww_init(const eidolon_mww_config_t* config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    uint8_t cutoff = 128;
    uint8_t window = 10;
    if (config) {
        if (config->probability_cutoff > 0) {
            cutoff = config->probability_cutoff;
        }
        if (config->sliding_window_size > 0) {
            window = config->sliding_window_size;
        }
        if (config->cooldown_ms > 0) {
            s_cooldown_ms = config->cooldown_ms;
        }
    }

    if (!InitFrontend(16000)) {
        return ESP_FAIL;
    }

    s_detector.model_start = hey_jarvis_tflite_start;
    s_detector.model_size = static_cast<size_t>(hey_jarvis_tflite_end - hey_jarvis_tflite_start);
    s_detector.probability_cutoff = cutoff;
    s_detector.sliding_window_size = window;
    s_detector.recent_probs.assign(window, 0);
    s_detector.ignore_windows = -static_cast<int>(kMinSlicesBeforeDetection);

    if (!s_detector.Load()) {
        return ESP_FAIL;
    }

    s_initialized = true;
    s_pending_detection = false;
    s_last_detect_us = 0;
    ESP_LOGI(TAG, "microWakeWord ready (model %zu bytes, cutoff=%u, window=%u)",
             s_detector.model_size, cutoff, window);
    return ESP_OK;
}

extern "C" void eidolon_mww_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    s_detector.Unload();
    if (s_frontend_ready) {
        FrontendFreeStateContents(&s_frontend_state);
        s_frontend_ready = false;
    }
    s_initialized = false;
}

extern "C" void eidolon_mww_reset(void)
{
    if (!s_initialized) {
        return;
    }
    s_detector.ignore_windows = -static_cast<int>(kMinSlicesBeforeDetection);
    std::fill(s_detector.recent_probs.begin(), s_detector.recent_probs.end(), 0);
    s_pending_detection = false;
}

extern "C" void eidolon_mww_feed_pcm(const int16_t* samples, size_t count)
{
    if (!s_initialized || !samples || count == 0) {
        return;
    }

    static std::vector<int16_t> pcm_buffer;
    pcm_buffer.insert(pcm_buffer.end(), samples, samples + count);

    const size_t step_samples = 16000 * kFeaturesStepMs / 1000;  // 160 @ 10ms
    int8_t features[kPreprocessorFeatureSize];

    while (pcm_buffer.size() >= step_samples) {
        size_t processed = GenerateFeatures(pcm_buffer.data(), step_samples, features);
        if (processed == 0) {
            break;
        }
        pcm_buffer.erase(pcm_buffer.begin(), pcm_buffer.begin() + processed);

        if (!s_detector.Infer(features)) {
            ESP_LOGW(TAG, "Inference error");
            break;
        }

        if (s_detector.CheckDetected()) {
            int64_t now = esp_timer_get_time();
            if (s_last_detect_us == 0 ||
                (now - s_last_detect_us) >= static_cast<int64_t>(s_cooldown_ms) * 1000) {
                s_last_detect_us = now;
                s_pending_detection = true;
                ESP_LOGI(TAG, "hey_jarvis detected");
            }
        }
    }
}

extern "C" bool eidolon_mww_poll_detected(void)
{
    if (!s_pending_detection) {
        return false;
    }
    s_pending_detection = false;
    return true;
}
