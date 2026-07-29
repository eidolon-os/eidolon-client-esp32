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

class GuardService {
public:
    explicit GuardService(Camera* camera);
    ~GuardService();

    bool Start(const GuardRuntimeConfig& config, const char* reason);
    void Stop(const char* reason);
    void SetObservationCallback(GuardObservationCallback callback);
    void SetOwnerPresenceCallback(OwnerPresenceCallback callback);

    GuardObservation CurrentObservation() const;
    OwnerPresenceObservation CurrentOwnerPresence() const;
    uint32_t OwnerPresenceLeaseMs() const;
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
    uint32_t owner_presence_lease_ms_ = 30000;
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
    void ApplyDisplayState(const GuardObservation& observation);
    uint64_t NowMs() const;
};

}  // namespace eidolon
