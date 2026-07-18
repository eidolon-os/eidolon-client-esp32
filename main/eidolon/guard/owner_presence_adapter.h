#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "guard/owner_presence_state_machine.h"

namespace eidolon {

struct OwnerPresenceRuntime {
    std::string guard_companion_id;
    std::string device_id;
    uint32_t boot_nonce = 0;

    bool usable() const { return !guard_companion_id.empty() && !device_id.empty(); }
};

class OwnerPresenceAdapter {
public:
    void Configure(OwnerPresenceRuntime runtime);
    void Clear();
    std::optional<std::string> Build(const OwnerPresenceObservation& observation,
                                     uint64_t ts_ms) const;

private:
    std::string CorrelationId(const OwnerPresenceObservation& observation) const;
    OwnerPresenceRuntime runtime_;
};

}  // namespace eidolon
