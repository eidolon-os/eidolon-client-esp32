#pragma once
// A single-threaded queue with FreeRTOS's copy-in/copy-out semantics, enough for
// the view's sense inbox (posted and drained on the same thread in the test).
#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>
#include "freertos/FreeRTOS.h"

struct HostQueue {
    size_t item_size;
    size_t capacity;
    std::deque<std::vector<uint8_t>> items;
};
typedef HostQueue* QueueHandle_t;

inline QueueHandle_t xQueueCreate(size_t capacity, size_t item_size) {
    return new HostQueue{item_size, capacity, {}};
}
inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
    if (!queue || queue->items.size() >= queue->capacity) return pdFALSE;
    const auto* bytes = static_cast<const uint8_t*>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t) {
    if (!queue || queue->items.empty()) return pdFALSE;
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}
