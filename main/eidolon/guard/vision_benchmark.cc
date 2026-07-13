#include "guard/vision_benchmark.h"

#include <cstdint>
#include <cstdio>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "boards/common/camera.h"
#include "guard/guard_motion.h"

namespace {

constexpr char kTag[] = "GuardVisionProbe";
constexpr uint32_t kMotionThreshold = 18;

struct HeapSnapshot {
    size_t heap = 0;
    size_t internal = 0;
    size_t psram = 0;
};

HeapSnapshot ReadHeap() {
    return {
        .heap = heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    };
}

void UpdateMinimum(HeapSnapshot& minimum, const HeapSnapshot& current) {
    minimum.heap = minimum.heap < current.heap ? minimum.heap : current.heap;
    minimum.internal = minimum.internal < current.internal ? minimum.internal : current.internal;
    minimum.psram = minimum.psram < current.psram ? minimum.psram : current.psram;
}

}  // namespace

std::string GuardVisionBenchmark::Run(Camera& camera, int sample_count, int interval_ms) {
    eidolon::GuardLuminanceGrid previous = {};
    eidolon::GuardLuminanceGrid current = {};
    bool has_previous = false;
    int captured = 0;
    int failed = 0;
    int motion_samples = 0;
    int motion_detected = 0;
    uint32_t motion_sum = 0;
    uint32_t motion_peak = 0;
    int64_t capture_us_sum = 0;
    int64_t capture_us_peak = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t pixel_format = 0;

    const HeapSnapshot heap_before = ReadHeap();
    HeapSnapshot heap_minimum = heap_before;
    const int64_t started_at_us = esp_timer_get_time();

    for (int index = 0; index < sample_count; ++index) {
        const int64_t capture_started_at_us = esp_timer_get_time();
        bool grid_ready = false;
        const bool frame_ok = camera.AnalyzeFrame([&](const CameraFrame& frame) {
            width = frame.width;
            height = frame.height;
            pixel_format = frame.pixel_format;
            grid_ready = eidolon::ReadGuardLuminanceGrid(frame, current);
            return grid_ready;
        });
        const int64_t capture_elapsed_us = esp_timer_get_time() - capture_started_at_us;

        if (!frame_ok || !grid_ready) {
            ++failed;
        } else {
            ++captured;
            capture_us_sum += capture_elapsed_us;
            capture_us_peak = capture_elapsed_us > capture_us_peak ? capture_elapsed_us : capture_us_peak;
            if (has_previous) {
                const uint32_t score = eidolon::GuardMotionScore(previous, current);
                motion_sum += score;
                motion_peak = score > motion_peak ? score : motion_peak;
                ++motion_samples;
                if (score >= kMotionThreshold) {
                    ++motion_detected;
                }
            }
            previous = current;
            has_previous = true;
        }

        UpdateMinimum(heap_minimum, ReadHeap());
        if (index + 1 < sample_count) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }

    const HeapSnapshot heap_after = ReadHeap();
    const int64_t elapsed_us = esp_timer_get_time() - started_at_us;
    char fourcc[5] = {};
    eidolon::GuardFourcc(pixel_format, fourcc);
    char json[768] = {};
    std::snprintf(
        json, sizeof(json),
        "{\"probe\":\"guard_vision_v1\",\"samples_requested\":%d,\"samples_captured\":%d,"
        "\"capture_failures\":%d,\"elapsed_ms\":%lu,\"capture_ms_avg\":%lu,\"capture_ms_peak\":%lu,"
        "\"input\":{\"width\":%u,\"height\":%u,\"pixel_format\":\"%s\"},"
        "\"motion\":{\"grid\":\"24x18\",\"threshold\":%lu,\"samples\":%d,\"mean\":%lu,"
        "\"peak\":%lu,\"detected\":%d},"
        "\"memory\":{\"heap_before\":%u,\"heap_minimum\":%u,\"heap_after\":%u,"
        "\"internal_before\":%u,\"internal_minimum\":%u,\"internal_after\":%u,"
        "\"psram_before\":%u,\"psram_minimum\":%u,\"psram_after\":%u}}",
        sample_count, captured, failed, static_cast<unsigned long>(elapsed_us / 1000),
        static_cast<unsigned long>(captured == 0 ? 0 : capture_us_sum / captured / 1000),
        static_cast<unsigned long>(capture_us_peak / 1000), width, height, fourcc,
        static_cast<unsigned long>(kMotionThreshold), motion_samples,
        static_cast<unsigned long>(motion_samples == 0 ? 0 : motion_sum / motion_samples),
        static_cast<unsigned long>(motion_peak), motion_detected,
        static_cast<unsigned>(heap_before.heap), static_cast<unsigned>(heap_minimum.heap), static_cast<unsigned>(heap_after.heap),
        static_cast<unsigned>(heap_before.internal), static_cast<unsigned>(heap_minimum.internal), static_cast<unsigned>(heap_after.internal),
        static_cast<unsigned>(heap_before.psram), static_cast<unsigned>(heap_minimum.psram), static_cast<unsigned>(heap_after.psram));
    ESP_LOGI(kTag, "%s", json);
    return json;
}
