#include "commissioning_orchestrator_core.h"

namespace eidolon {

void CommissioningOrchestratorCore::Act(
    std::vector<CommissioningAction>& actions,
    CommissioningActionType type) const
{
    actions.push_back({type, generation_, candidate_id_});
}

void CommissioningOrchestratorCore::Transition(
    std::vector<CommissioningAction>& actions,
    CommissioningRuntimeState state)
{
    state_ = state;
    Act(actions, CommissioningActionType::PublishConfirmedState);
}

void CommissioningOrchestratorCore::BeginReturn(
    std::vector<CommissioningAction>& actions)
{
    if (state_ == CommissioningRuntimeState::ReturningToPreviousMode ||
        state_ == CommissioningRuntimeState::RestoringPreviousMode) return;
    Transition(actions, CommissioningRuntimeState::ReturningToPreviousMode);
    if (!stop_requested_) {
        stop_requested_ = true;
        Act(actions, CommissioningActionType::StopTransport);
    }
}

void CommissioningOrchestratorCore::BeginRestore(
    std::vector<CommissioningAction>& actions)
{
    if (state_ == CommissioningRuntimeState::ReturningToPreviousMode ||
        state_ == CommissioningRuntimeState::RestoringPreviousMode) return;
    Transition(actions, CommissioningRuntimeState::RestoringPreviousMode);
    if ((trust_staged_ || candidate_staged_) && !transaction_committed_) {
        if (!rollback_requested_) {
            rollback_requested_ = true;
            Act(actions, CommissioningActionType::RollbackCommissioningTransaction);
        }
    } else if (!stop_requested_) {
        stop_requested_ = true;
        Act(actions, CommissioningActionType::StopTransport);
    }
}

void CommissioningOrchestratorCore::ResetToIdle(
    std::vector<CommissioningAction>& actions)
{
    candidate_id_.clear();
    trust_staged_ = false;
    candidate_staged_ = false;
    owner_validation_requested_ = false;
    owner_route_validated_ = false;
    commit_requested_ = false;
    transaction_committed_ = false;
    rollback_requested_ = false;
    stop_requested_ = false;
    radio_restore_requested_ = false;
    Transition(actions, CommissioningRuntimeState::Idle);
}

std::vector<CommissioningAction> CommissioningOrchestratorCore::Handle(
    const CommissioningEvent& event)
{
    std::vector<CommissioningAction> actions;
    if (event.type == CommissioningEventType::OpenRequested) {
        if (state_ != CommissioningRuntimeState::Idle) return actions;
        ++generation_;
        if (generation_ == 0) ++generation_;
        Transition(actions, CommissioningRuntimeState::PreparingIdentity);
        Act(actions, CommissioningActionType::EnsureIdentity);
        return actions;
    }
    if (event.generation != generation_ ||
        state_ == CommissioningRuntimeState::Idle) {
        return actions;
    }

    switch (event.type) {
    case CommissioningEventType::IdentityReady:
        if (state_ == CommissioningRuntimeState::PreparingIdentity) {
            Transition(actions, CommissioningRuntimeState::AcquiringRadio);
            Act(actions, CommissioningActionType::AcquireCommissioningRadioLease);
        }
        break;
    case CommissioningEventType::RadioAcquired:
        if (state_ == CommissioningRuntimeState::AcquiringRadio) {
            Transition(actions, CommissioningRuntimeState::StartingTransport);
            Act(actions, CommissioningActionType::StartTransport);
        }
        break;
    case CommissioningEventType::TransportReady:
        if (state_ == CommissioningRuntimeState::StartingTransport &&
            !event.transport_ready.advertisement_id.empty() &&
            event.transport_ready.endpoints_registered &&
            event.transport_ready.security_ready) {
            Transition(actions, CommissioningRuntimeState::Advertising);
        }
        break;
    case CommissioningEventType::AuthenticatedSessionStarted:
        if (state_ == CommissioningRuntimeState::Advertising) {
            Transition(actions, CommissioningRuntimeState::SessionActive);
        }
        break;
    case CommissioningEventType::TrustStaged:
        if (state_ == CommissioningRuntimeState::SessionActive) {
            trust_staged_ = true;
            Act(actions, CommissioningActionType::PublishConfirmedState);
        }
        break;
    case CommissioningEventType::TrustStageFailed:
        if (state_ == CommissioningRuntimeState::SessionActive) {
            BeginRestore(actions);
        }
        break;
    case CommissioningEventType::NetworkCandidateReceived:
        if (state_ == CommissioningRuntimeState::SessionActive && trust_staged_ &&
            !event.candidate_id.empty()) {
            candidate_id_ = event.candidate_id;
            Transition(actions, CommissioningRuntimeState::ApplyingConfiguration);
            Act(actions, CommissioningActionType::StageNetworkCandidate);
        }
        break;
    case CommissioningEventType::NetworkCandidateStaged:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            event.candidate_id == candidate_id_) {
            candidate_staged_ = true;
        }
        break;
    case CommissioningEventType::WifiConnected:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            candidate_staged_ && !owner_validation_requested_ &&
            event.candidate_id == candidate_id_) {
            owner_validation_requested_ = true;
            Act(actions, CommissioningActionType::ValidateOwnerRoute);
        }
        break;
    case CommissioningEventType::OwnerRouteValidated:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            candidate_staged_ && !commit_requested_ &&
            event.candidate_id == candidate_id_) {
            owner_route_validated_ = true;
            commit_requested_ = true;
            Act(actions, CommissioningActionType::CommitCommissioningTransaction);
        }
        break;
    case CommissioningEventType::CommissioningTransactionCommitted:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            trust_staged_ && owner_route_validated_ && !transaction_committed_ &&
            event.candidate_id == candidate_id_) {
            transaction_committed_ = true;
            Act(actions, CommissioningActionType::PublishConfirmedState);
        }
        break;
    case CommissioningEventType::CommissioningTransactionCommitFailed:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            event.candidate_id == candidate_id_) {
            BeginRestore(actions);
        }
        break;
    case CommissioningEventType::ControllerObservedTerminal:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            transaction_committed_) {
            BeginReturn(actions);
        }
        break;
    case CommissioningEventType::OwnerRouteValidationFailed:
        if (state_ == CommissioningRuntimeState::ApplyingConfiguration &&
            event.candidate_id == candidate_id_) {
            BeginRestore(actions);
        }
        break;
    case CommissioningEventType::CommissioningTransactionRolledBack:
        if (state_ == CommissioningRuntimeState::RestoringPreviousMode &&
            event.candidate_id == candidate_id_) {
            trust_staged_ = false;
            candidate_staged_ = false;
            if (!stop_requested_) {
                stop_requested_ = true;
                Act(actions, CommissioningActionType::StopTransport);
            }
        }
        break;
    case CommissioningEventType::WindowExpired:
    case CommissioningEventType::CancelRequested:
        if (transaction_committed_) BeginReturn(actions);
        else BeginRestore(actions);
        break;
    case CommissioningEventType::IdentityFailed:
    case CommissioningEventType::RadioAcquisitionFailed:
        // Neither failure acquired a transport or a radio lease. There is
        // nothing to tear down and no callback that could truthfully confirm a
        // transport stop, so converge directly instead of waiting forever for
        // synthetic cleanup evidence.
        ResetToIdle(actions);
        break;
    case CommissioningEventType::TransportStartFailed:
        BeginRestore(actions);
        break;
    case CommissioningEventType::TransportEndedUnexpectedly:
        // The SDK may report an unsolicited transport end from its event task.
        // It is evidence only: the actor still owns the one idempotent cleanup
        // path for this generation.
        if (transaction_committed_) BeginReturn(actions);
        else BeginRestore(actions);
        break;
    case CommissioningEventType::TransportStopped:
        if (state_ == CommissioningRuntimeState::ReturningToPreviousMode ||
            state_ == CommissioningRuntimeState::RestoringPreviousMode) {
            if (radio_restore_requested_) break;
            radio_restore_requested_ = true;
            Act(actions, CommissioningActionType::RestorePreviousRadioMode);
        }
        break;
    case CommissioningEventType::PreviousModeRestored:
        if (state_ == CommissioningRuntimeState::ReturningToPreviousMode ||
            state_ == CommissioningRuntimeState::RestoringPreviousMode) {
            // A committed act does not merely restore a Wi-Fi mode: it must
            // prove the new Station route is usable before commissioning may
            // hand control to enrollment/admission.
            if (transaction_committed_) break;
            ResetToIdle(actions);
        }
        break;
    case CommissioningEventType::StationRouteReady:
        if ((state_ == CommissioningRuntimeState::ReturningToPreviousMode ||
             state_ == CommissioningRuntimeState::RestoringPreviousMode) &&
            transaction_committed_) {
            ResetToIdle(actions);
        }
        break;
    case CommissioningEventType::OpenRequested:
        break;
    }
    return actions;
}

}  // namespace eidolon
