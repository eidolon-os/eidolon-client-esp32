#include "guard/owner_presence_state_machine.h"

#include <algorithm>

namespace eidolon {

const char* OwnerPresenceStateName(OwnerPresenceState state)
{
    switch (state) {
    case OwnerPresenceState::Unavailable: return "unavailable";
    case OwnerPresenceState::Watching: return "watching";
    case OwnerPresenceState::PresentPending: return "present_pending";
    case OwnerPresenceState::Present: return "present";
    case OwnerPresenceState::AbsentPending: return "absent_pending";
    }
    return "unknown";
}

const char* OwnerPresenceFactName(OwnerPresenceFact fact)
{
    switch (fact) {
    case OwnerPresenceFact::None: return "none";
    case OwnerPresenceFact::Present: return "present";
    case OwnerPresenceFact::Absent: return "absent";
    }
    return "none";
}

void OwnerPresenceStateMachine::ApplyConfig(const OwnerPresenceConfig& config)
{
    config_ = config;
    config_.enter_ms = std::max<uint32_t>(config_.enter_ms, 500);
    config_.exit_ms = std::max<uint32_t>(config_.exit_ms, config_.enter_ms);
    config_.heartbeat_ms = std::max<uint32_t>(config_.heartbeat_ms, 1000);
    config_.lease_ms = std::max<uint32_t>(config_.lease_ms, config_.heartbeat_ms + 1000);
}

OwnerPresenceObservation OwnerPresenceStateMachine::Process(const OwnerPresenceSample& sample)
{
    if (!sample.profile_active || sample.profile_revision == 0 ||
        sample.profile_revision != profile_revision_) {
        return ResetProfile(sample.profile_active, sample.profile_revision, sample.now_ms);
    }
    if (sample.face_match) {
        identity_session_active_ = true;
        return ObserveOwner(sample.now_ms, true);
    }
    // A detected face below the owner threshold is absence of positive owner
    // evidence, not proof that a different person replaced the owner. The local
    // database contains owner templates only, so a non-match must not transfer
    // or revoke an already face-gated identity session.
    if (sample.person_evaluated && sample.person_present &&
        identity_session_active_) {
        return ObserveOwner(sample.now_ms, false);
    }
    if (!sample.face_evaluated && !sample.person_evaluated) {
        return Observation(sample.now_ms);
    }

    if (state_ == OwnerPresenceState::PresentPending && sample.person_evaluated &&
        !sample.person_present) {
        state_ = OwnerPresenceState::Watching;
        pending_since_ms_ = 0;
        identity_session_active_ = false;
    } else if (state_ == OwnerPresenceState::Present && sample.person_evaluated &&
               !sample.person_present) {
        state_ = OwnerPresenceState::AbsentPending;
    } else if (state_ == OwnerPresenceState::AbsentPending &&
               sample.now_ms - last_match_ms_ >= config_.exit_ms) {
        state_ = OwnerPresenceState::Watching;
        pending_since_ms_ = 0;
        last_present_fact_ms_ = 0;
        identity_session_active_ = false;
        return Emit(OwnerPresenceFact::Absent, profile_revision_, sample.now_ms);
    }
    return Observation(sample.now_ms);
}

OwnerPresenceObservation OwnerPresenceStateMachine::Stop(uint64_t now_ms)
{
    const uint32_t old_revision = profile_revision_;
    const bool was_present = state_ == OwnerPresenceState::Present ||
                             state_ == OwnerPresenceState::AbsentPending;
    state_ = OwnerPresenceState::Unavailable;
    profile_revision_ = 0;
    pending_since_ms_ = 0;
    last_match_ms_ = 0;
    last_present_fact_ms_ = 0;
    identity_session_active_ = false;
    return was_present ? Emit(OwnerPresenceFact::Absent, old_revision, now_ms)
                       : Observation(now_ms);
}

OwnerPresenceObservation OwnerPresenceStateMachine::Current(uint64_t now_ms) const
{
    return Observation(now_ms);
}

OwnerPresenceObservation OwnerPresenceStateMachine::ResetProfile(bool active, uint32_t revision,
                                                                  uint64_t now_ms)
{
    const uint32_t old_revision = profile_revision_;
    const bool was_present = state_ == OwnerPresenceState::Present ||
                             state_ == OwnerPresenceState::AbsentPending;
    profile_revision_ = active ? revision : 0;
    state_ = active ? OwnerPresenceState::Watching : OwnerPresenceState::Unavailable;
    pending_since_ms_ = 0;
    last_match_ms_ = 0;
    last_present_fact_ms_ = 0;
    identity_session_active_ = false;
    return was_present ? Emit(OwnerPresenceFact::Absent, old_revision, now_ms)
                       : Observation(now_ms);
}

OwnerPresenceObservation OwnerPresenceStateMachine::Emit(OwnerPresenceFact fact,
                                                          uint32_t fact_profile_revision,
                                                          uint64_t now_ms)
{
    ++sequence_;
    OwnerPresenceObservation value = Observation(now_ms);
    value.fact = fact;
    value.fact_profile_revision = fact_profile_revision;
    value.lease_ms = fact == OwnerPresenceFact::Present ? config_.lease_ms : 0;
    return value;
}

OwnerPresenceObservation OwnerPresenceStateMachine::Observation(uint64_t now_ms) const
{
    return {
        .state = state_,
        .fact = OwnerPresenceFact::None,
        .profile_revision = profile_revision_,
        .fact_profile_revision = 0,
        .epoch = epoch_,
        .sequence = sequence_,
        .now_ms = now_ms,
        .last_match_ms = last_match_ms_,
        .lease_ms = 0,
        .identity_session_active = identity_session_active_,
    };
}

OwnerPresenceObservation OwnerPresenceStateMachine::ObserveOwner(
    uint64_t now_ms, bool identity_evidence)
{
    last_match_ms_ = now_ms;
    if (state_ == OwnerPresenceState::Watching && identity_evidence) {
        state_ = OwnerPresenceState::PresentPending;
        pending_since_ms_ = now_ms;
    } else if (state_ == OwnerPresenceState::PresentPending &&
               now_ms - pending_since_ms_ >= config_.enter_ms) {
        state_ = OwnerPresenceState::Present;
        ++epoch_;
        last_present_fact_ms_ = now_ms;
        return Emit(OwnerPresenceFact::Present, profile_revision_, now_ms);
    } else if (state_ == OwnerPresenceState::AbsentPending) {
        state_ = OwnerPresenceState::Present;
    }
    if (state_ == OwnerPresenceState::Present &&
        now_ms - last_present_fact_ms_ >= config_.heartbeat_ms) {
        last_present_fact_ms_ = now_ms;
        return Emit(OwnerPresenceFact::Present, profile_revision_, now_ms);
    }
    return Observation(now_ms);
}

}  // namespace eidolon
