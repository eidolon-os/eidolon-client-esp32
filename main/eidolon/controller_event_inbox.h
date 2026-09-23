#pragma once

#include <cstdint>
#include <type_traits>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace eidolon {

// The controller owns task-notification index 0. Ordered events stay in the
// FreeRTOS FIFO; these two level-triggered requests carry no history or payload.
// Producers and the consumer must be stopped before destruction.
template<class Event>
class ControllerEventInbox {
public:
    static constexpr uint32_t kAudioTick = 1U << 0;
    static constexpr uint32_t kSessionActivity = 1U << 1;
    static constexpr uint32_t kQueueReady = 1U << 2;

    explicit ControllerEventInbox(UBaseType_t capacity)
        : queue_(xQueueCreate(capacity, sizeof(Event))) {
        static_assert(std::is_trivially_copyable_v<Event>);
    }
    ~ControllerEventInbox() { if (queue_) vQueueDelete(queue_); }
    ControllerEventInbox(const ControllerEventInbox&) = delete;
    ControllerEventInbox& operator=(const ControllerEventInbox&) = delete;
    bool Valid() const { return queue_ != nullptr; }
    UBaseType_t Pending() const { return uxQueueMessagesWaiting(queue_); }

    bool Post(const Event& event, TaskHandle_t consumer) {
        if (!queue_ || !consumer || xQueueSend(queue_, &event, 0) != pdTRUE) return false;
        xTaskNotify(consumer, kQueueReady, eSetBits);
        return true;
    }
    static void Signal(TaskHandle_t consumer, uint32_t bits) {
        configASSERT(consumer != nullptr);
        xTaskNotify(consumer, bits, eSetBits);
    }
    bool Take(Event& event) { return xQueueReceive(queue_, &event, 0) == pdTRUE; }

    // Call on the consumer only. A post between the empty check and the wait
    // leaves a notification pending, so it cannot be lost. FIFO backlog avoids
    // sleeping even when multiple QueueReady notifications have coalesced.
    // The caller handles at most one FIFO event and each signal per iteration.
    uint32_t Wait() {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, Pending() ? 0 : portMAX_DELAY);
        return bits & (kAudioTick | kSessionActivity);
    }

private:
    QueueHandle_t queue_;
};

} // namespace eidolon
