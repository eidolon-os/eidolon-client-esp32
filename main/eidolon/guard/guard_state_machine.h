#pragma once

#include <cstdint>

namespace eidolon {

enum class GuardState {
    Disabled,
    Idle,
    CandidatePending,
    Candidate,
    AbsentPending,
    Absent,
    Fault,
};

enum class GuardFaultCode {
    None,
    CameraUnavailable,
    CaptureFailures,
};

struct GuardRuntimeConfig {
    uint32_t sample_interval_ms = 500;
    uint32_t preview_interval_ms = 1000;
    uint32_t motion_threshold = 18;
    uint32_t motion_clear_threshold = 9;
    uint32_t candidate_debounce_ms = 1000;
    uint32_t absence_timeout_ms = 180000;
    uint32_t consecutive_capture_failures = 5;
    uint32_t owner_face_interval_ms = 1500;
    uint32_t owner_presence_enter_ms = 2500;
    uint32_t owner_presence_exit_ms = 12000;
    uint32_t owner_presence_heartbeat_ms = 10000;
    uint32_t owner_presence_lease_ms = 30000;
};

struct GuardSample {
    uint64_t now_ms = 0;
    bool frame_ok = false;
    bool motion_valid = false;
    uint32_t motion_score = 0;
};

struct GuardObservation {
    uint32_t epoch = 0;
    uint32_t sequence = 0;
    GuardState state = GuardState::Disabled;
    GuardFaultCode fault = GuardFaultCode::None;
    uint32_t motion_score = 0;
    uint64_t now_ms = 0;
    uint64_t last_seen_ms = 0;
};

const char* GuardStateName(GuardState state);
const char* GuardFaultCodeName(GuardFaultCode fault);

class GuardStateMachine {
public:
    void ApplyConfig(const GuardRuntimeConfig& config);
    const GuardRuntimeConfig& config() const { return config_; }

    GuardObservation Start(uint64_t now_ms);
    GuardObservation Stop(uint64_t now_ms);
    GuardObservation Fault(uint64_t now_ms, GuardFaultCode fault);
    GuardObservation ProcessSample(const GuardSample& sample);

    GuardObservation Current(uint64_t now_ms) const;
    GuardState state() const { return state_; }

private:
    GuardRuntimeConfig config_;
    GuardState state_ = GuardState::Disabled;
    GuardFaultCode fault_ = GuardFaultCode::None;
    uint32_t epoch_ = 0;
    uint32_t sequence_ = 0;
    uint32_t consecutive_failures_ = 0;
    uint64_t pending_since_ms_ = 0;
    uint64_t last_seen_ms_ = 0;

    bool HasMotion(const GuardSample& sample) const;
    bool HasCleared(const GuardSample& sample) const;
    GuardObservation Transition(GuardState state, uint64_t now_ms, uint32_t motion_score,
                                GuardFaultCode fault = GuardFaultCode::None);
    GuardObservation Observation(uint64_t now_ms, uint32_t motion_score) const;
};

}  // namespace eidolon
