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
    bool station_mode_requested = false;
    uint32_t revision = 0;
};

// ESP-IDF composition root for the platform-independent commissioning Core.
// Board, timer and SDK callbacks only submit events here; the dedicated actor
// is the sole component that advances a setup generation and owns its leases.
class CommissioningRuntime {
public:
    using Observer = std::function<void(const CommissioningRuntimeSnapshot&)>;
    // The application-owned operational plane must release voice/media and
    // other radio consumers before commissioning may acquire its RadioLease.
    // Completion is evidence; issuing a disconnect command is not.
    using OperationalRuntimeQuiescer =
        std::function<void(std::function<void(bool)>)>;

    static CommissioningRuntime& GetInstance();

    bool RequestOpen();
    bool RequestCancel();
    // Scheduled projections must still belong to the latest actor evidence.
    bool IsCurrent(const CommissioningRuntimeSnapshot& snapshot) const;
    void SetObserver(Observer observer);
    void SetOperationalRuntimeQuiescer(OperationalRuntimeQuiescer quiescer);
    bool IsAdvertising() const;
    uint32_t Generation() const;
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
