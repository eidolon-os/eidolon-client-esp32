#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "guard/guard_state_machine.h"

namespace eidolon {

// Identity and non-sensitive runtime values obtained from the signed Guard
// runtime-config endpoint. This deliberately contains no owner or media data.
struct GuardPresenceRuntime {
    std::string guard_companion_id;
    std::string device_id;
    uint32_t runtime_revision = 0;
    uint32_t candidate_debounce_ms = 0;
    uint32_t boot_nonce = 0;

    bool usable() const
    {
        return !guard_companion_id.empty() && !device_id.empty() && runtime_revision > 0;
    }
};

// Pure adapter from local, aggregate Guard observations to the versioned P0
// fact contract. It has no transport dependency: the controller owns queueing
// and LiveKit publication on its single actor task.
class GuardPresenceAdapter {
public:
    void Configure(GuardPresenceRuntime runtime);
    void Clear();

    // Produces facts only on Candidate and Absent transitions. Repeated state
    // callbacks for one Guard epoch are deduplicated locally.
    std::optional<std::string> Build(const GuardObservation& observation, uint64_t ts_ms);

private:
    std::string CorrelationId(uint32_t guard_epoch) const;

    GuardPresenceRuntime runtime_;
    bool candidate_emitted_ = false;
    uint32_t candidate_epoch_ = 0;
    bool absence_emitted_ = false;
    uint32_t absence_epoch_ = 0;
};

}  // namespace eidolon
