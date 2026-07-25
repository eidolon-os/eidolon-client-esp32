#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "boards/common/camera.h"
#include "guard/guard_motion.h"
#include "guard/guard_state_machine.h"
#include "guard/owner_recognition_flow.h"
#include "guard/owner_presence_state_machine.h"
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
#include "guard/owner_face_engine.h"
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
#include "guard/person_presence_engine.h"
#endif

namespace eidolon {

class GuardDisplay;

using GuardObservationCallback = std::function<void(const GuardObservation& observation)>;
using OwnerPresenceCallback =
    std::function<void(const OwnerPresenceObservation& observation)>;

struct OwnerRecognitionConfirmation {
    std::string flow_id;
    std::string causation_id;
    uint32_t profile_revision = 0;
    uint32_t guard_epoch = 0;
    uint32_t presence_sequence = 0;
    uint64_t confirmed_at_ms = 0;
    uint32_t request_generation = 0;
};

using OwnerRecognitionCallback =
    std::function<void(const OwnerRecognitionConfirmation& confirmation)>;

class GuardService {
public:
    explicit GuardService(Camera* camera);
    ~GuardService();

    bool Start(const GuardRuntimeConfig& config, const char* reason);
    void Stop(const char* reason);
    void SetObservationCallback(GuardObservationCallback callback);
    void SetOwnerPresenceCallback(OwnerPresenceCallback callback);
    void SetOwnerRecognitionCallback(OwnerRecognitionCallback callback);
    OwnerRecognitionRequestResult RequestOwnerRecognition(
        const std::string& flow_id, const std::string& causation_id,
        uint32_t request_generation);

    GuardObservation CurrentObservation() const;
    std::string StatusJson() const;
    bool IsRunning() const;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    OwnerFaceEngine* owner_face_engine() const { return owner_face_engine_.get(); }
#endif

    static GuardRuntimeConfig DefaultConfig();

private:
    Camera* camera_ = nullptr;
    mutable std::mutex mutex_;
    GuardStateMachine state_machine_;
    GuardObservation last_observation_;
    GuardObservationCallback callback_;
    OwnerPresenceStateMachine owner_presence_state_machine_;
    OwnerPresenceObservation last_owner_presence_observation_;
    OwnerPresenceCallback owner_presence_callback_;
    OwnerRecognitionFlowTracker owner_recognition_flows_;
    OwnerRecognitionCallback owner_recognition_callback_;
    TaskHandle_t task_handle_ = nullptr;
    bool running_ = false;
    bool task_exit_ = false;
    uint32_t published_sequence_ = 0;

#if !CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
    GuardLuminanceGrid previous_grid_ = {};
    bool has_previous_grid_ = false;
    bool reset_grid_requested_ = false;
#endif
    std::unique_ptr<GuardDisplay> guard_display_;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    std::unique_ptr<OwnerFaceEngine> owner_face_engine_;
    OwnerFaceProfileStatus last_owner_face_profile_status_;
    OwnerFaceLiveResult last_owner_face_result_;
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
    std::unique_ptr<PersonPresenceEngine> person_presence_engine_;
    PersonPresenceResult last_person_presence_result_;
#endif

    static void TaskTrampoline(void* arg);
    void TaskLoop();
    GuardSample CaptureSample(uint64_t now_ms);
    void PublishObservation(const GuardObservation& observation);
    void PublishOwnerPresence(const OwnerPresenceObservation& observation);
    void PublishOwnerRecognitionConfirmations(
        const std::vector<OwnerRecognitionFlow>& completed,
        const OwnerPresenceObservation& owner_presence);
    void ApplyDisplayState(const GuardObservation& observation);
    uint64_t NowMs() const;
};

}  // namespace eidolon
