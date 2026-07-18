#include "guard/owner_presence_adapter.h"

#include <cstdio>
#include <utility>

#include "eidolon_topics.h"
#include "guard/guard_wire_json.h"

namespace eidolon {

void OwnerPresenceAdapter::Configure(OwnerPresenceRuntime runtime)
{
    runtime_ = std::move(runtime);
}

void OwnerPresenceAdapter::Clear()
{
    runtime_ = OwnerPresenceRuntime{};
}

std::optional<std::string> OwnerPresenceAdapter::Build(
    const OwnerPresenceObservation& observation, uint64_t ts_ms) const
{
    if (!runtime_.usable() || observation.fact == OwnerPresenceFact::None ||
        observation.fact_profile_revision == 0 || observation.sequence == 0) {
        return std::nullopt;
    }
    const char* state = OwnerPresenceFactName(observation.fact);
    return std::string("{\"type\":\"") + kGuardOwnerPresenceType +
           "\",\"schema_v\":1,\"guard_companion_id\":\"" +
           GuardJsonEscape(runtime_.guard_companion_id) + "\",\"device_id\":\"" +
           GuardJsonEscape(runtime_.device_id) + "\",\"correlation_id\":\"" +
           GuardJsonEscape(CorrelationId(observation)) + "\",\"guard_epoch\":" +
           std::to_string(observation.epoch) + ",\"ts_ms\":" + std::to_string(ts_ms) +
           ",\"state\":\"" + state + "\",\"profile_revision\":" +
           std::to_string(observation.fact_profile_revision) + ",\"sequence\":" +
           std::to_string(observation.sequence) + ",\"lease_ms\":" +
           std::to_string(observation.lease_ms) + ",\"raw_retention\":\"none\"}";
}

std::string OwnerPresenceAdapter::CorrelationId(
    const OwnerPresenceObservation& observation) const
{
    char value[64] = {};
    std::snprintf(value, sizeof(value), "op-%08lx-r%lu-e%lu",
                  static_cast<unsigned long>(runtime_.boot_nonce),
                  static_cast<unsigned long>(observation.fact_profile_revision),
                  static_cast<unsigned long>(observation.epoch));
    return value;
}

}  // namespace eidolon
