#ifndef EIDOLON_AMBIENT_PRESENCE_STATE_H_
#define EIDOLON_AMBIENT_PRESENCE_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eidolon {

enum class AmbientPresenceObservation {
    Edge,
    Snapshot,
    Heartbeat,
};

struct AmbientPresenceState {
    bool present = false;
    uint32_t presence_epoch = 0;
    uint32_t sequence = 0;
    uint32_t lease_ms = 0;
    AmbientPresenceObservation observation = AmbientPresenceObservation::Edge;
};

struct AmbientPresenceAssertion {
    std::string source_device_id;
    std::string flow_id;
    std::string root_event_id;
    uint32_t presence_epoch = 0;
    uint32_t sequence = 0;
    uint64_t source_occurred_at_ms = 0;
    uint64_t lease_deadline_ms = 0;
    bool active = false;
    bool owner_confirmed = false;
};

enum class AmbientPresenceApplyResult {
    Rejected,
    Stale,
    Armed,
    Refreshed,
    Disarmed,
};

enum class AmbientOwnerLifecycleApplyResult {
    Rejected,
    Stale,
    Present,
    Absent,
};

// BOX-side activation idempotency is intentionally independent of the ATK
// registry. It prevents receiver restart/state repair from consuming the same
// physical radar epoch twice, while a signed owner departure can reopen it.
class AmbientPresenceActivationGate {
public:
    void ResetForRadarTransition();
    bool TryConsume(uint32_t presence_epoch,
                    const std::string& owner_authority_device_id,
                    uint64_t owner_fact_occurred_at_ms,
                    uint32_t guard_epoch, uint32_t presence_sequence);
    AmbientOwnerLifecycleApplyResult ApplyOwnerLifecycle(
        const std::string& source_device_id, bool present,
        uint64_t occurred_at_ms, uint32_t guard_epoch,
        uint32_t presence_sequence);

private:
    uint32_t consumed_epoch_ = 0;
    std::string owner_authority_device_id_;
    uint64_t owner_fact_occurred_at_ms_ = 0;
    uint32_t owner_fact_guard_epoch_ = 0;
    uint32_t owner_fact_sequence_ = 0;
};

// Source-scoped presence assertions are long-lived local state, not bounded
// owner-recognition requests. Hub events update this registry; local visual
// owner facts consume the active assertions whenever the owner appears.
class AmbientPresenceRegistry {
public:
    static constexpr size_t kMaxSources = 4;

    AmbientPresenceApplyResult Apply(
        const std::string& source_device_id, const std::string& flow_id,
        const std::string& event_id, const AmbientPresenceState& state,
        uint64_t source_occurred_at_ms, uint64_t received_at_ms);

    std::vector<AmbientPresenceAssertion> PendingOwnerConfirmations() const;
    bool MarkOwnerConfirmed(const std::string& source_device_id,
                            uint32_t presence_epoch);
    void OnOwnerAbsent();
    std::vector<AmbientPresenceAssertion> Expire(uint64_t now_ms);
    void DeactivateAll();
    uint64_t NextLeaseDeadlineMs() const;
    bool HasActive() const;

private:
    std::array<AmbientPresenceAssertion, kMaxSources> assertions_ = {};
    size_t assertion_count_ = 0;

    size_t FindSource(const std::string& source_device_id) const;
    size_t AllocateSource();
};

const char* AmbientPresenceObservationName(
    AmbientPresenceObservation observation);

}  // namespace eidolon

#endif  // EIDOLON_AMBIENT_PRESENCE_STATE_H_
