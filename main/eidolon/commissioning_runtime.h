#ifndef EIDOLON_COMMISSIONING_RUNTIME_H_
#define EIDOLON_COMMISSIONING_RUNTIME_H_

#include "commissioning_orchestrator_core.h"

#include <cstdint>
#include <functional>

namespace eidolon {

struct CommissioningRuntimeSnapshot {
    CommissioningRuntimeState state = CommissioningRuntimeState::Idle;
    uint32_t generation = 0;
    bool transaction_committed = false;
    bool station_route_ready = false;
    bool previous_station_mode = false;
};

// ESP-IDF composition root for the platform-independent commissioning Core.
// Board, timer and SDK callbacks only submit events here; the dedicated actor
// is the sole component that advances a setup generation and owns its leases.
class CommissioningRuntime {
public:
    using Observer = std::function<void(const CommissioningRuntimeSnapshot&)>;

    static CommissioningRuntime& GetInstance();

    bool RequestOpen();
    bool RequestCancel();
    // The board adapter submits confirmed Station connectivity here. Returning
    // true transfers this event to the commissioning actor, which prevents the
    // legacy network callback from starting enrollment before the actor has
    // released its generation.
    bool NotifyStationRouteReady();
    void SetObserver(Observer observer);
    bool IsAdvertising() const;
    // True for the whole physical commissioning lease, including identity
    // preparation, transport startup and restoration. Network callbacks use
    // this projection only to avoid presenting an expected Station handover as
    // an unrelated offline failure; they never advance commissioning state.
    bool IsInProgress() const;

private:
    CommissioningRuntime() = default;
};

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_RUNTIME_H_
