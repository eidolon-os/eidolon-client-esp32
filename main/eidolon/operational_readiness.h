#ifndef EIDOLON_OPERATIONAL_READINESS_H_
#define EIDOLON_OPERATIONAL_READINESS_H_

#include <cstdint>

namespace eidolon {

// An observed route is handed to the operational plane once per connection
// and setup generation. A completed setup may have quiesced that plane even
// when the network adapter reconnects before Application processes the handoff.
inline bool ShouldResumeOperationalNetwork(
    bool connected, bool previously_connected, uint32_t generation,
    uint32_t previous_generation, bool commissioning_in_progress)
{
    return connected && !commissioning_in_progress &&
           (!previously_connected || generation != previous_generation);
}

// Host/chip-independent projection boundary. Registration/config state and a
// successful connect() call are deliberately absent: Operational is confirmed
// only by the transport's asynchronous ready evidence for the current route.
struct OperationalReadinessSnapshot {
    bool owner_network_ready = false;
    bool hub_activation_ready = false;
    bool transport_ready = false;
    bool commissioning_in_progress = false;
};

inline bool IsOperationalConfirmed(const OperationalReadinessSnapshot& snapshot)
{
    return snapshot.owner_network_ready && snapshot.hub_activation_ready &&
           snapshot.transport_ready && !snapshot.commissioning_in_progress;
}

}  // namespace eidolon

#endif  // EIDOLON_OPERATIONAL_READINESS_H_
