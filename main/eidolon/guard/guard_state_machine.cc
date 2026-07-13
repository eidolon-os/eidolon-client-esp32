#include "guard/guard_state_machine.h"

namespace eidolon {

const char* GuardStateName(GuardState state)
{
    switch (state) {
    case GuardState::Disabled:
        return "disabled";
    case GuardState::Idle:
        return "idle";
    case GuardState::CandidatePending:
        return "candidate_pending";
    case GuardState::Candidate:
        return "candidate";
    case GuardState::AbsentPending:
        return "absent_pending";
    case GuardState::Absent:
        return "absent";
    case GuardState::Fault:
        return "fault";
    }
    return "unknown";
}

const char* GuardFaultCodeName(GuardFaultCode fault)
{
    switch (fault) {
    case GuardFaultCode::None:
        return "none";
    case GuardFaultCode::CameraUnavailable:
        return "camera_unavailable";
    case GuardFaultCode::CaptureFailures:
        return "capture_failures";
    }
    return "unknown";
}

void GuardStateMachine::ApplyConfig(const GuardRuntimeConfig& config)
{
    config_ = config;
    if (config_.sample_interval_ms < 200) {
        config_.sample_interval_ms = 200;
    }
    if (config_.preview_interval_ms < config_.sample_interval_ms) {
        config_.preview_interval_ms = config_.sample_interval_ms;
    }
    if (config_.motion_clear_threshold > config_.motion_threshold) {
        config_.motion_clear_threshold = config_.motion_threshold;
    }
    if (config_.candidate_debounce_ms < config_.sample_interval_ms) {
        config_.candidate_debounce_ms = config_.sample_interval_ms;
    }
    if (config_.absence_timeout_ms < config_.sample_interval_ms * 2) {
        config_.absence_timeout_ms = config_.sample_interval_ms * 2;
    }
    if (config_.consecutive_capture_failures == 0) {
        config_.consecutive_capture_failures = 1;
    }
}

GuardObservation GuardStateMachine::Start(uint64_t now_ms)
{
    ++epoch_;
    consecutive_failures_ = 0;
    pending_since_ms_ = 0;
    last_seen_ms_ = 0;
    return Transition(GuardState::Idle, now_ms, 0);
}

GuardObservation GuardStateMachine::Stop(uint64_t now_ms)
{
    consecutive_failures_ = 0;
    pending_since_ms_ = 0;
    last_seen_ms_ = 0;
    return Transition(GuardState::Disabled, now_ms, 0);
}

GuardObservation GuardStateMachine::Fault(uint64_t now_ms, GuardFaultCode fault)
{
    return Transition(GuardState::Fault, now_ms, 0, fault);
}

GuardObservation GuardStateMachine::ProcessSample(const GuardSample& sample)
{
    if (state_ == GuardState::Disabled || state_ == GuardState::Fault) {
        return Observation(sample.now_ms, sample.motion_score);
    }

    if (!sample.frame_ok || !sample.motion_valid) {
        ++consecutive_failures_;
        if (consecutive_failures_ >= config_.consecutive_capture_failures) {
            return Transition(GuardState::Fault, sample.now_ms, sample.motion_score,
                              GuardFaultCode::CaptureFailures);
        }
        return Observation(sample.now_ms, sample.motion_score);
    }
    consecutive_failures_ = 0;

    switch (state_) {
    case GuardState::Idle:
        if (HasMotion(sample)) {
            ++epoch_;
            pending_since_ms_ = sample.now_ms;
            last_seen_ms_ = sample.now_ms;
            return Transition(GuardState::CandidatePending, sample.now_ms, sample.motion_score);
        }
        break;
    case GuardState::CandidatePending:
        if (HasCleared(sample)) {
            pending_since_ms_ = 0;
            last_seen_ms_ = 0;
            return Transition(GuardState::Idle, sample.now_ms, sample.motion_score);
        }
        if (HasMotion(sample)) {
            last_seen_ms_ = sample.now_ms;
        }
        if (sample.now_ms - pending_since_ms_ >= config_.candidate_debounce_ms) {
            return Transition(GuardState::Candidate, sample.now_ms, sample.motion_score);
        }
        break;
    case GuardState::Candidate:
        if (HasMotion(sample)) {
            last_seen_ms_ = sample.now_ms;
        } else if (HasCleared(sample)) {
            return Transition(GuardState::AbsentPending, sample.now_ms, sample.motion_score);
        }
        break;
    case GuardState::AbsentPending:
        if (HasMotion(sample)) {
            last_seen_ms_ = sample.now_ms;
            return Transition(GuardState::Candidate, sample.now_ms, sample.motion_score);
        }
        if (last_seen_ms_ != 0 && sample.now_ms - last_seen_ms_ >= config_.absence_timeout_ms) {
            return Transition(GuardState::Absent, sample.now_ms, sample.motion_score);
        }
        break;
    case GuardState::Absent:
        if (HasMotion(sample)) {
            ++epoch_;
            pending_since_ms_ = sample.now_ms;
            last_seen_ms_ = sample.now_ms;
            return Transition(GuardState::CandidatePending, sample.now_ms, sample.motion_score);
        }
        break;
    case GuardState::Disabled:
    case GuardState::Fault:
        break;
    }

    return Observation(sample.now_ms, sample.motion_score);
}

GuardObservation GuardStateMachine::Current(uint64_t now_ms) const
{
    return Observation(now_ms, 0);
}

bool GuardStateMachine::HasMotion(const GuardSample& sample) const
{
    return sample.motion_valid && sample.motion_score >= config_.motion_threshold;
}

bool GuardStateMachine::HasCleared(const GuardSample& sample) const
{
    return sample.motion_valid && sample.motion_score <= config_.motion_clear_threshold;
}

GuardObservation GuardStateMachine::Transition(GuardState state, uint64_t now_ms,
                                               uint32_t motion_score, GuardFaultCode fault)
{
    state_ = state;
    fault_ = fault;
    ++sequence_;
    return Observation(now_ms, motion_score);
}

GuardObservation GuardStateMachine::Observation(uint64_t now_ms, uint32_t motion_score) const
{
    return {
        .epoch = epoch_,
        .sequence = sequence_,
        .state = state_,
        .fault = fault_,
        .motion_score = motion_score,
        .now_ms = now_ms,
        .last_seen_ms = last_seen_ms_,
    };
}

}  // namespace eidolon
