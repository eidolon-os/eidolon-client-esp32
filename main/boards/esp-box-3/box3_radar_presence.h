#pragma once

#include <atomic>
#include <functional>

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "radar_presence_tracker.h"

class Box3RadarPresence {
public:
    using StateCallback = std::function<void(RadarPresenceState)>;

    Box3RadarPresence() = default;
    ~Box3RadarPresence();

    esp_err_t Start(i2c_master_bus_handle_t sensor_bus, StateCallback callback);

private:
    static void TaskEntry(void* arg);
    void Run();
    bool ProbeSensorDock();
    void PublishState(RadarPresenceState state);

    i2c_master_bus_handle_t sensor_bus_ = nullptr;
    StateCallback callback_;
    RadarPresenceTracker tracker_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_ = false;
    RadarPresenceState published_state_ = RadarPresenceState::Unavailable;
    bool has_published_state_ = false;
    bool raw_detected_ = false;
    bool has_raw_detected_ = false;
    unsigned consecutive_probe_failures_ = 0;
};
