#ifndef EIDOLON_OPERATIONAL_READINESS_H_
#define EIDOLON_OPERATIONAL_READINESS_H_

namespace eidolon {

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
