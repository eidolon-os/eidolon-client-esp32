#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "boards/common/camera.h"
#include "guard/guard_motion.h"
#include "guard/guard_state_machine.h"

namespace eidolon {

class GuardDisplay;

using GuardObservationCallback = std::function<void(const GuardObservation& observation)>;

class GuardService {
public:
    explicit GuardService(Camera* camera);
    ~GuardService();

    bool Start(const GuardRuntimeConfig& config, const char* reason);
    void Stop(const char* reason);
    void SetObservationCallback(GuardObservationCallback callback);

    GuardObservation CurrentObservation() const;
    std::string StatusJson() const;
    bool IsRunning() const;

    static GuardRuntimeConfig DefaultConfig();

private:
    Camera* camera_ = nullptr;
    mutable std::mutex mutex_;
    GuardStateMachine state_machine_;
    GuardObservation last_observation_;
    GuardObservationCallback callback_;
    TaskHandle_t task_handle_ = nullptr;
    bool running_ = false;
    bool task_exit_ = false;
    uint32_t published_sequence_ = 0;

    GuardLuminanceGrid previous_grid_ = {};
    bool has_previous_grid_ = false;
    bool reset_grid_requested_ = false;
    std::unique_ptr<GuardDisplay> guard_display_;

    static void TaskTrampoline(void* arg);
    void TaskLoop();
    GuardSample CaptureSample(uint64_t now_ms);
    void PublishObservation(const GuardObservation& observation);
    void ApplyDisplayState(const GuardObservation& observation);
    uint64_t NowMs() const;
};

}  // namespace eidolon
