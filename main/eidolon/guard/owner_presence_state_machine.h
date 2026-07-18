#pragma once

#include <cstdint>

namespace eidolon {

enum class OwnerPresenceState {
    Unavailable,
    Watching,
    PresentPending,
    Present,
    AbsentPending,
};

enum class OwnerPresenceFact {
    None,
    Present,
    Absent,
};

struct OwnerPresenceConfig {
    uint32_t enter_ms = 2500;
    uint32_t exit_ms = 12000;
    uint32_t heartbeat_ms = 10000;
    uint32_t lease_ms = 30000;
};

struct OwnerPresenceSample {
    uint64_t now_ms = 0;
    bool profile_active = false;
    uint32_t profile_revision = 0;
    bool face_evaluated = false;
    bool face_match = false;
    bool person_evaluated = false;
    bool person_present = false;
};

struct OwnerPresenceObservation {
    OwnerPresenceState state = OwnerPresenceState::Unavailable;
    OwnerPresenceFact fact = OwnerPresenceFact::None;
    uint32_t profile_revision = 0;
    uint32_t fact_profile_revision = 0;
    uint32_t epoch = 0;
    uint32_t sequence = 0;
    uint64_t now_ms = 0;
    uint64_t last_match_ms = 0;
    uint32_t lease_ms = 0;
    bool identity_session_active = false;
};

const char* OwnerPresenceStateName(OwnerPresenceState state);
const char* OwnerPresenceFactName(OwnerPresenceFact fact);

class OwnerPresenceStateMachine {
public:
    void ApplyConfig(const OwnerPresenceConfig& config);
    OwnerPresenceObservation Process(const OwnerPresenceSample& sample);
    OwnerPresenceObservation Stop(uint64_t now_ms);
    OwnerPresenceObservation Current(uint64_t now_ms) const;

private:
    OwnerPresenceConfig config_;
    OwnerPresenceState state_ = OwnerPresenceState::Unavailable;
    uint32_t profile_revision_ = 0;
    uint32_t epoch_ = 0;
    uint32_t sequence_ = 0;
    uint64_t pending_since_ms_ = 0;
    uint64_t last_match_ms_ = 0;
    uint64_t last_present_fact_ms_ = 0;
    bool identity_session_active_ = false;

    OwnerPresenceObservation Emit(OwnerPresenceFact fact, uint32_t fact_profile_revision,
                                  uint64_t now_ms);
    OwnerPresenceObservation Observation(uint64_t now_ms) const;
    OwnerPresenceObservation ResetProfile(bool active, uint32_t revision, uint64_t now_ms);
    OwnerPresenceObservation ObserveOwner(uint64_t now_ms, bool identity_evidence);
};

}  // namespace eidolon
