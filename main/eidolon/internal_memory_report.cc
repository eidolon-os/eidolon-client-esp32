#include "internal_memory_report.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#define TAG "EidolonMem"

namespace eidolon {

InternalHeapSnapshot CaptureInternalHeap()
{
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    InternalHeapSnapshot snapshot;
    snapshot.free_bytes = info.total_free_bytes;
    snapshot.largest_free_block = info.largest_free_block;
    snapshot.min_free_bytes = info.minimum_free_bytes;
    snapshot.free_blocks = info.free_blocks;
    return snapshot;
}

void LogInternalHeapSummary(const char* phase)
{
    const InternalHeapSnapshot internal = CaptureInternalHeap();
    multi_heap_info_t internal_info = {};
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);

    ESP_LOGW(TAG,
             "[mem] %s internal free=%u largest_block=%u free_blocks=%u "
             "allocated=%u min_free=%u",
             phase ? phase : "?",
             static_cast<unsigned>(internal.free_bytes),
             static_cast<unsigned>(internal.largest_free_block),
             static_cast<unsigned>(internal.free_blocks),
             static_cast<unsigned>(internal_info.total_allocated_bytes),
             static_cast<unsigned>(internal.min_free_bytes));

    // The DMA-capable subset separately: the LVGL draw buffer, the codec's I2S
    // descriptors and the WiFi static buffers all live here, and none of them can
    // be moved to PSRAM the way an ordinary buffer can.
    multi_heap_info_t dma = {};
    heap_caps_get_info(&dma, MALLOC_CAP_DMA);
    ESP_LOGW(TAG, "[mem] %s internal-dma free=%u largest_block=%u allocated=%u",
             phase ? phase : "?",
             static_cast<unsigned>(dma.total_free_bytes),
             static_cast<unsigned>(dma.largest_free_block),
             static_cast<unsigned>(dma.total_allocated_bytes));

    // PSRAM alongside it, because the point of every fix in this area is moving
    // a block from the scarce heap to the abundant one, and a reader needs both
    // numbers to see that a move was worth making.
    multi_heap_info_t spiram = {};
    heap_caps_get_info(&spiram, MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "[mem] %s psram free=%u largest_block=%u",
             phase ? phase : "?",
             static_cast<unsigned>(spiram.total_free_bytes),
             static_cast<unsigned>(spiram.largest_free_block));
}

void LogInternalStackLedger(const char* phase)
{
    const UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        return;
    }
    // From PSRAM: an accounting of the internal heap must not itself take a
    // kilobyte out of the internal heap it is about to report on.
    auto* tasks = static_cast<TaskStatus_t*>(heap_caps_calloc(
        task_count, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tasks == nullptr) {
        ESP_LOGW(TAG, "[mem] %s stack ledger unavailable (%u tasks)", phase ? phase : "?",
                 static_cast<unsigned>(task_count));
        return;
    }

    const UBaseType_t filled = uxTaskGetSystemState(tasks, task_count, nullptr);
    unsigned internal_stacks = 0;
    unsigned external_stacks = 0;
    for (UBaseType_t i = 0; i < filled; ++i) {
        const TaskStatus_t& task = tasks[i];
        if (task.pxStackBase == nullptr) {
            continue;
        }
        if (esp_ptr_external_ram(task.pxStackBase)) {
            ++external_stacks;
            continue;
        }
        ++internal_stacks;
        // usStackHighWaterMark is in bytes on ESP-IDF (not words, as in vanilla
        // FreeRTOS). A large headroom on an internal stack is reclaimable
        // internal RAM; that is the whole reason this line exists.
        ESP_LOGW(TAG, "[mem] %s stack task=%s prio=%u unused=%u bytes",
                 phase ? phase : "?", task.pcTaskName ? task.pcTaskName : "?",
                 static_cast<unsigned>(task.uxCurrentPriority),
                 static_cast<unsigned>(task.usStackHighWaterMark));
    }
    ESP_LOGW(TAG, "[mem] %s stacks internal=%u psram=%u total_tasks=%u",
             phase ? phase : "?", internal_stacks, external_stacks,
             static_cast<unsigned>(filled));
    heap_caps_free(tasks);
}

void LogInternalMemoryLedger(const char* phase)
{
    LogInternalHeapSummary(phase);
    LogInternalStackLedger(phase);
}

}  // namespace eidolon
