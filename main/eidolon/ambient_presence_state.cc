#include "ambient_presence_state.h"

#include <algorithm>

namespace eidolon {
namespace {

constexpr size_t kNotFound = AmbientPresenceRegistry::kMaxSources;

bool IsBoundedId(const std::string& value)
{
    return !value.empty() && value.size() <= 128;
}

}  // namespace

const char* AmbientPresenceObservationName(
    AmbientPresenceObservation observation)
{
    switch (observation) {
    case AmbientPresenceObservation::Edge:
        return "edge";
    case AmbientPresenceObservation::Snapshot:
        return "snapshot";
    case AmbientPresenceObservation::Heartbeat:
        return "heartbeat";
    }
    return "unknown";
}

void AmbientPresenceActivationGate::ResetForRadarTransition()
{
    consumed_epoch_ = 0;
    owner_authority_device_id_.clear();
    owner_fact_occurred_at_ms_ = 0;
    owner_fact_guard_epoch_ = 0;
    owner_fact_sequence_ = 0;
}

bool AmbientPresenceActivationGate::TryConsume(
    uint32_t presence_epoch,
    const std::string& owner_authority_device_id,
    uint64_t owner_fact_occurred_at_ms, uint32_t guard_epoch,
    uint32_t presence_sequence)
{
    if (presence_epoch == 0 ||
        !IsBoundedId(owner_authority_device_id) ||
        owner_fact_occurred_at_ms == 0 || presence_sequence == 0 ||
        consumed_epoch_ == presence_epoch) {
        return false;
    }
    consumed_epoch_ = presence_epoch;
    owner_authority_device_id_ = owner_authority_device_id;
    owner_fact_occurred_at_ms_ = owner_fact_occurred_at_ms;
    owner_fact_guard_epoch_ = guard_epoch;
    owner_fact_sequence_ = presence_sequence;
    return true;
}

AmbientOwnerLifecycleApplyResult
AmbientPresenceActivationGate::ApplyOwnerLifecycle(
    const std::string& source_device_id, bool present,
    uint64_t occurred_at_ms, uint32_t guard_epoch,
    uint32_t presence_sequence)
{
    if (owner_authority_device_id_.empty() ||
        source_device_id != owner_authority_device_id_ ||
        occurred_at_ms == 0 || presence_sequence == 0) {
        return AmbientOwnerLifecycleApplyResult::Rejected;
    }
    if (occurred_at_ms < owner_fact_occurred_at_ms_ ||
        (occurred_at_ms == owner_fact_occurred_at_ms_ &&
         (guard_epoch < owner_fact_guard_epoch_ ||
          (guard_epoch == owner_fact_guard_epoch_ &&
           presence_sequence <= owner_fact_sequence_)))) {
        return AmbientOwnerLifecycleApplyResult::Stale;
    }
    owner_fact_occurred_at_ms_ = occurred_at_ms;
    owner_fact_guard_epoch_ = guard_epoch;
    owner_fact_sequence_ = presence_sequence;
    if (present) {
        return AmbientOwnerLifecycleApplyResult::Present;
    }
    consumed_epoch_ = 0;
    return AmbientOwnerLifecycleApplyResult::Absent;
}

AmbientPresenceApplyResult AmbientPresenceRegistry::Apply(
    const std::string& source_device_id, const std::string& flow_id,
    const std::string& event_id, const AmbientPresenceState& state,
    uint64_t source_occurred_at_ms, uint64_t received_at_ms)
{
    if (!IsBoundedId(source_device_id) || !IsBoundedId(flow_id) ||
        !IsBoundedId(event_id) || source_occurred_at_ms == 0 ||
        state.presence_epoch == 0 ||
        state.sequence == 0 ||
        (state.present && (state.lease_ms == 0 || state.lease_ms > 60000)) ||
        (!state.present && state.lease_ms != 0)) {
        return AmbientPresenceApplyResult::Rejected;
    }

    size_t index = FindSource(source_device_id);
    if (index == kNotFound) {
        index = AllocateSource();
        assertions_[index].source_device_id = source_device_id;
    }
    auto& assertion = assertions_[index];
    if (assertion.source_occurred_at_ms != 0 &&
        source_occurred_at_ms < assertion.source_occurred_at_ms) {
        return AmbientPresenceApplyResult::Stale;
    }
    const bool publisher_restarted =
        assertion.source_occurred_at_ms != 0 &&
        source_occurred_at_ms > assertion.source_occurred_at_ms &&
        flow_id != assertion.flow_id &&
        state.presence_epoch <= assertion.presence_epoch;
    if (!publisher_restarted &&
        (state.presence_epoch < assertion.presence_epoch ||
        (state.presence_epoch == assertion.presence_epoch &&
         state.sequence <= assertion.sequence))) {
        return AmbientPresenceApplyResult::Stale;
    }
    if (publisher_restarted) {
        assertion = {};
        assertion.source_device_id = source_device_id;
    }

    const bool new_epoch = state.presence_epoch != assertion.presence_epoch;
    const bool was_active = assertion.active;
    assertion.presence_epoch = state.presence_epoch;
    assertion.sequence = state.sequence;
    assertion.source_occurred_at_ms = source_occurred_at_ms;

    if (!state.present) {
        assertion.active = false;
        assertion.owner_confirmed = false;
        assertion.lease_deadline_ms = 0;
        return AmbientPresenceApplyResult::Disarmed;
    }

    assertion.active = true;
    assertion.lease_deadline_ms = received_at_ms + state.lease_ms;
    if (new_epoch) {
        assertion.owner_confirmed = false;
    }
    if (new_epoch || !was_active ||
        state.observation == AmbientPresenceObservation::Edge ||
        assertion.flow_id.empty() || assertion.root_event_id.empty()) {
        assertion.flow_id = flow_id;
        assertion.root_event_id = event_id;
        return AmbientPresenceApplyResult::Armed;
    }
    // A snapshot or heartbeat repairs the lease only. It must never reopen an
    // already-consumed owner confirmation or behave like a new radar edge.
    return AmbientPresenceApplyResult::Refreshed;
}

std::vector<AmbientPresenceAssertion>
AmbientPresenceRegistry::PendingOwnerConfirmations() const
{
    std::vector<AmbientPresenceAssertion> pending;
    pending.reserve(assertion_count_);
    for (size_t i = 0; i < assertion_count_; ++i) {
        if (assertions_[i].active && !assertions_[i].owner_confirmed) {
            pending.push_back(assertions_[i]);
        }
    }
    return pending;
}

bool AmbientPresenceRegistry::MarkOwnerConfirmed(
    const std::string& source_device_id, uint32_t presence_epoch)
{
    const size_t index = FindSource(source_device_id);
    if (index == kNotFound || !assertions_[index].active ||
        assertions_[index].presence_epoch != presence_epoch) {
        return false;
    }
    assertions_[index].owner_confirmed = true;
    return true;
}

void AmbientPresenceRegistry::OnOwnerAbsent()
{
    for (size_t i = 0; i < assertion_count_; ++i) {
        // Preserve a same-epoch confirmation across lease repair so a normal
        // Voice Room end cannot immediately re-open itself. A real local owner
        // departure is the authority that reopens the assertion, even if its
        // lease happened to expire while BOX was away in the Voice Room.
        assertions_[i].owner_confirmed = false;
    }
}

std::vector<AmbientPresenceAssertion>
AmbientPresenceRegistry::Expire(uint64_t now_ms)
{
    std::vector<AmbientPresenceAssertion> expired;
    for (size_t i = 0; i < assertion_count_; ++i) {
        auto& assertion = assertions_[i];
        if (!assertion.active || assertion.lease_deadline_ms == 0 ||
            now_ms < assertion.lease_deadline_ms) {
            continue;
        }
        expired.push_back(assertion);
        assertion.active = false;
        assertion.lease_deadline_ms = 0;
    }
    return expired;
}

void AmbientPresenceRegistry::DeactivateAll()
{
    for (size_t i = 0; i < assertion_count_; ++i) {
        assertions_[i].active = false;
        assertions_[i].lease_deadline_ms = 0;
    }
}

uint64_t AmbientPresenceRegistry::NextLeaseDeadlineMs() const
{
    uint64_t deadline = 0;
    for (size_t i = 0; i < assertion_count_; ++i) {
        const auto& assertion = assertions_[i];
        if (!assertion.active || assertion.lease_deadline_ms == 0) {
            continue;
        }
        deadline = deadline == 0
                       ? assertion.lease_deadline_ms
                       : std::min(deadline, assertion.lease_deadline_ms);
    }
    return deadline;
}

bool AmbientPresenceRegistry::HasActive() const
{
    for (size_t i = 0; i < assertion_count_; ++i) {
        if (assertions_[i].active) {
            return true;
        }
    }
    return false;
}

size_t AmbientPresenceRegistry::FindSource(
    const std::string& source_device_id) const
{
    for (size_t i = 0; i < assertion_count_; ++i) {
        if (assertions_[i].source_device_id == source_device_id) {
            return i;
        }
    }
    return kNotFound;
}

size_t AmbientPresenceRegistry::AllocateSource()
{
    if (assertion_count_ < assertions_.size()) {
        return assertion_count_++;
    }
    // Prefer replacing an inactive source. If all four are active, replace the
    // assertion whose lease expires first; owner-scoped desktop deployments
    // should not normally reach this bounded embedded back-pressure path.
    for (size_t i = 0; i < assertion_count_; ++i) {
        if (!assertions_[i].active) {
            assertions_[i] = {};
            return i;
        }
    }
    size_t oldest = 0;
    for (size_t i = 1; i < assertion_count_; ++i) {
        if (assertions_[i].lease_deadline_ms <
            assertions_[oldest].lease_deadline_ms) {
            oldest = i;
        }
    }
    assertions_[oldest] = {};
    return oldest;
}

}  // namespace eidolon
