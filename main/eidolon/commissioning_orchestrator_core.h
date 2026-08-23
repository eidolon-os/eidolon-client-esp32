#ifndef EIDOLON_COMMISSIONING_ORCHESTRATOR_CORE_H_
#define EIDOLON_COMMISSIONING_ORCHESTRATOR_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace eidolon {

enum class CommissioningRuntimeState {
    Idle,
    PreparingIdentity,
    AcquiringRadio,
    StartingTransport,
    Advertising,
    SessionActive,
    ApplyingConfiguration,
    ReturningToPreviousMode,
    RestoringPreviousMode,
};

enum class CommissioningEventType {
    OpenRequested,
    IdentityReady,
    IdentityFailed,
    RadioAcquired,
    RadioAcquisitionFailed,
    TransportReady,
    TransportStartFailed,
    TransportEndedUnexpectedly,
    AuthenticatedSessionStarted,
    TrustStaged,
    TrustStageFailed,
    NetworkCandidateReceived,
    NetworkCandidateStaged,
    WifiConnected,
    OwnerRouteValidated,
    OwnerRouteValidationFailed,
    CommissioningTransactionCommitted,
    CommissioningTransactionCommitFailed,
    CommissioningTransactionRolledBack,
    ControllerObservedTerminal,
    WindowExpired,
    CancelRequested,
    TransportStopped,
    PreviousModeRestored,
    StationRouteReady,
};

struct TransportReadyEvidence {
    std::string advertisement_id;
    bool endpoints_registered = false;
    bool security_ready = false;
};

struct CommissioningEvent {
    CommissioningEventType type = CommissioningEventType::OpenRequested;
    uint32_t generation = 0;
    std::string candidate_id;
    TransportReadyEvidence transport_ready;
};

enum class CommissioningActionType {
    EnsureIdentity,
    AcquireCommissioningRadioLease,
    StartTransport,
    StageNetworkCandidate,
    ValidateOwnerRoute,
    CommitCommissioningTransaction,
    RollbackCommissioningTransaction,
    StopTransport,
    RestorePreviousRadioMode,
    PublishConfirmedState,
};

struct CommissioningAction {
    CommissioningActionType type;
    uint32_t generation;
    std::string candidate_id;
};

// Host-, chip-, radio- and transport-independent single-writer state machine.
// Platform callbacks enqueue CommissioningEvent; only the actor consuming this
// Core may execute the returned actions.
class CommissioningOrchestratorCore {
public:
    std::vector<CommissioningAction> Handle(const CommissioningEvent& event);

    CommissioningRuntimeState state() const { return state_; }
    uint32_t generation() const { return generation_; }
    bool transaction_committed() const { return transaction_committed_; }

private:
    void Act(std::vector<CommissioningAction>& actions,
             CommissioningActionType type) const;
    void Transition(std::vector<CommissioningAction>& actions,
                    CommissioningRuntimeState state);
    void BeginRestore(std::vector<CommissioningAction>& actions);
    void BeginReturn(std::vector<CommissioningAction>& actions);
    void ResetToIdle(std::vector<CommissioningAction>& actions);

    CommissioningRuntimeState state_ = CommissioningRuntimeState::Idle;
    uint32_t generation_ = 0;
    std::string candidate_id_;
    bool trust_staged_ = false;
    bool candidate_staged_ = false;
    bool owner_validation_requested_ = false;
    bool owner_route_validated_ = false;
    bool commit_requested_ = false;
    bool transaction_committed_ = false;
    bool rollback_requested_ = false;
    bool stop_requested_ = false;
    bool radio_restore_requested_ = false;
};

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_ORCHESTRATOR_CORE_H_
