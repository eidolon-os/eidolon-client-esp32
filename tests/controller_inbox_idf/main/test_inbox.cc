#include "eidolon/controller_event_inbox.h"
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cassert>
#include <atomic>

struct Event { int producer; int sequence; };
using Inbox = eidolon::ControllerEventInbox<Event>;
static constexpr auto tick = Inbox::kAudioTick;
static constexpr auto activity = Inbox::kSessionActivity;

static void saturation() {
    const auto self = xTaskGetCurrentTaskHandle();
    // Counterexample with the original transport, using the real RTOS queue.
    auto legacy = xQueueCreate(24, sizeof(Event));
    Event event{0, 0};
    for (int i = 0; i < 24; ++i) assert(xQueueSend(legacy, &event, 0) == pdTRUE);
    assert(xQueueSend(legacy, &event, 0) == pdFALSE); // control request rejected
    vQueueDelete(legacy);

    Inbox inbox(24);
    assert(inbox.Valid());
    for (int i = 0; i < 10000; ++i) inbox.Signal(self, tick | activity);
    assert(inbox.Pending() == 0);
    for (int i = 0; i < 24; ++i) assert(inbox.Post({0, i}, self));
    assert(!inbox.Post({0, 24}, self)); // genuine command pressure still rejects
    assert(inbox.Wait() == (tick | activity));
    assert(inbox.Take(event) && event.sequence == 0);
    // A signal during handling survives clear-on-exit for the next iteration.
    inbox.Signal(self, tick);
    assert(inbox.Wait() == tick);
    for (int i = 1; i < 24; ++i) {
        assert(inbox.Take(event) && event.sequence == i);
        if (i < 23) assert(inbox.Wait() == 0); // backlog must not sleep
    }
    assert(!inbox.Take(event));
    assert(inbox.Post({0, 25}, self)); // capacity recovers after a rejection
    assert(inbox.Wait() == 0);
    assert(inbox.Take(event) && event.sequence == 25);
    ESP_LOGI("INBOX_TEST", "legacy saturation reproduced; 10000 coalesced signals, FIFO/rejection/recovery PASS");
}

struct TimerScenario {
    Inbox* inbox;
    TaskHandle_t consumer;
    QueueHandle_t legacy;
    SemaphoreHandle_t done;
    esp_timer_handle_t timer = nullptr;
    int ticks = 0;
    int rejected = 0;
};
static void periodic_tick(void* arg) {
    auto& test = *static_cast<TimerScenario*>(arg);
    Event event{0, test.ticks};
    if (xQueueSend(test.legacy, &event, 0) != pdTRUE) ++test.rejected;
    test.inbox->Signal(test.consumer, tick);
    if (++test.ticks == 30) {
        assert(esp_timer_stop(test.timer) == ESP_OK);
        xSemaphoreGive(test.done);
    }
}
static void real_timer_stall() {
    Inbox inbox(24);
    TimerScenario test{&inbox, xTaskGetCurrentTaskHandle(),
                       xQueueCreate(24, sizeof(Event)), xSemaphoreCreateBinary()};
    esp_timer_create_args_t args{};
    args.callback = periodic_tick;
    args.arg = &test;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "inbox_contract";
    assert(esp_timer_create(&args, &test.timer) == ESP_OK);
    assert(esp_timer_start_periodic(test.timer, 80000) == ESP_OK);
    // Consumer intentionally does no dispatch for 30 real 80ms timer firings.
    assert(xSemaphoreTake(test.done, pdMS_TO_TICKS(4000)) == pdTRUE);
    assert(test.ticks == 30 && test.rejected == 6);
    Event command{1, 0};
    assert(xQueueSend(test.legacy, &command, 0) == pdFALSE);
    assert(inbox.Pending() == 0);
    for (int i = 0; i < 24; ++i) assert(inbox.Post({1, i}, test.consumer));
    assert(inbox.Wait() == tick);
    for (int i = 0; i < 24; ++i) {
        assert(inbox.Take(command) && command.sequence == i);
        if (i < 23) inbox.Wait();
    }
    assert(esp_timer_delete(test.timer) == ESP_OK);
    vSemaphoreDelete(test.done);
    vQueueDelete(test.legacy);
    ESP_LOGI("INBOX_TEST", "real 80ms esp_timer / 2400ms consumer stall PASS old_dropped_ticks=6 old_control=rejected new_controls=24/24");
}

struct Producer {
    Inbox* inbox;
    TaskHandle_t consumer;
    SemaphoreHandle_t done;
    int id;
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
};
static void produce(void* arg) {
    auto& p = *static_cast<Producer*>(arg);
    for (int i = 0; i < 2000; ++i) {
        p.inbox->Signal(p.consumer, tick | activity);
        if (p.inbox->Post({p.id, i}, p.consumer)) ++p.accepted;
        else ++p.rejected;
        if (i % 7 == 0) vTaskDelay(1);
    }
    // Explicit end markers use caller-side retry; ordinary production Post is
    // still non-blocking. The test never treats an unaccepted command as sent.
    while (!p.inbox->Post({p.id, 2000}, p.consumer)) vTaskDelay(1);
    xSemaphoreGive(p.done);
    vTaskDelete(nullptr);
}
static void concurrent_slow_consumer() {
    Inbox inbox(24);
    auto self = xTaskGetCurrentTaskHandle();
    Producer a{&inbox, self, xSemaphoreCreateBinary(), 0};
    Producer b{&inbox, self, xSemaphoreCreateBinary(), 1};
    assert(a.done && b.done);
    assert(xTaskCreate(produce, "producer_a", 3072, &a, 5, nullptr) == pdPASS);
    assert(xTaskCreate(produce, "producer_b", 3072, &b, 5, nullptr) == pdPASS);
    // Controlled consumer stall; no reset, watchdog disable or crash injection.
    vTaskDelay(pdMS_TO_TICKS(100));
    int last[2] = {-1, -1}, received[2] = {}, finished = 0, ticks = 0, activities = 0;
    while (finished < 2) {
        auto signals = inbox.Wait();
        Event event;
        if (inbox.Take(event)) {
            assert(event.sequence > last[event.producer]);
            last[event.producer] = event.sequence;
            if (event.sequence == 2000) ++finished;
            else ++received[event.producer];
        }
        if (signals & tick) ++ticks;
        if (signals & activity) ++activities;
        vTaskDelay(1);
    }
    assert(xSemaphoreTake(a.done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(xSemaphoreTake(b.done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(received[0] == a.accepted && received[1] == b.accepted);
    assert(a.accepted + a.rejected == 2000 && b.accepted + b.rejected == 2000);
    assert(a.rejected + b.rejected > 0 && ticks > 1 && activities > 1);
    vSemaphoreDelete(a.done); vSemaphoreDelete(b.done);
    ESP_LOGI("INBOX_TEST", "two producers/slow consumer PASS accepted=%d rejected=%d maintenance=%d/%d",
             received[0] + received[1], int(a.rejected + b.rejected), ticks, activities);
}

static void delayed_signal(void* arg) {
    auto* p = static_cast<Producer*>(arg);
    for (int i = 0; i < 1000; ++i) {
        // Alternates posts against an empty/sleeping consumer and a ready one.
        if (i % 2) vTaskDelay(1);
        while (!p->inbox->Post({0, i}, p->consumer)) vTaskDelay(1);
    }
    xSemaphoreGive(p->done);
    vTaskDelete(nullptr);
}
static void wakeups() {
    Inbox inbox(24);
    Producer p{&inbox, xTaskGetCurrentTaskHandle(), xSemaphoreCreateBinary(), 0};
    assert(xTaskCreate(delayed_signal, "wakeups", 3072, &p, 5, nullptr) == pdPASS);
    for (int i = 0; i < 1000;) {
        inbox.Wait();
        Event event;
        if (inbox.Take(event)) assert(event.sequence == i++);
    }
    assert(xSemaphoreTake(p.done, pdMS_TO_TICKS(1000)) == pdTRUE);
    vSemaphoreDelete(p.done);
    ESP_LOGI("INBOX_TEST", "1000 empty-check/wait wakeups PASS");
}
extern "C" void app_main() {
    saturation();
    real_timer_stall();
    concurrent_slow_consumer();
    wakeups();
    ESP_LOGI("INBOX_TEST", "ALL PASS");
}
