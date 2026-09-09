#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

#include "eidolon/commissioning_orchestrator_core.h"

namespace {

using namespace eidolon;

bool Has(const std::vector<CommissioningAction>& actions,
         CommissioningActionType type)
{
    return std::any_of(actions.begin(), actions.end(),
                       [type](const auto& action) { return action.type == type; });
}

CommissioningEvent Event(CommissioningEventType type, uint32_t generation,
                         const std::string& candidate = {})
{
    CommissioningEvent event;
    event.type = type;
    event.generation = generation;
    event.candidate_id = candidate;
    return event;
}

uint32_t OpenToSession(CommissioningOrchestratorCore& core)
{
    auto actions = core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    assert(Has(actions, CommissioningActionType::EnsureIdentity));
    const uint32_t generation = core.generation();
    core.Handle(Event(CommissioningEventType::IdentityReady, generation));
    core.Handle(Event(CommissioningEventType::OperationalRuntimeQuiesced,
                      generation));
    core.Handle(Event(CommissioningEventType::RadioAcquired, generation));
    auto ready = Event(CommissioningEventType::TransportReady, generation);
    ready.transport_ready = {"eidolon-a1b2c3", true, true};
    core.Handle(ready);
    assert(core.state() == CommissioningRuntimeState::Advertising);
    core.Handle(Event(CommissioningEventType::AuthenticatedSessionStarted,
                      generation));
    assert(core.state() == CommissioningRuntimeState::SessionActive);
    return generation;
}

void StageTrust(CommissioningOrchestratorCore& core, uint32_t generation)
{
    auto actions = core.Handle(Event(CommissioningEventType::TrustStaged,
                                     generation));
    assert(Has(actions, CommissioningActionType::PublishConfirmedState));
}

void TestReadyRequiresEvidenceAndCallbacksAreGenerationFenced()
{
    CommissioningOrchestratorCore core;
    core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    const uint32_t generation = core.generation();
    core.Handle(Event(CommissioningEventType::IdentityReady, generation));
    core.Handle(Event(CommissioningEventType::OperationalRuntimeQuiesced,
                      generation));
    core.Handle(Event(CommissioningEventType::RadioAcquired, generation));
    auto incomplete = Event(CommissioningEventType::TransportReady, generation);
    incomplete.transport_ready = {"eidolon-a1b2c3", true, false};
    assert(core.Handle(incomplete).empty());
    assert(core.state() == CommissioningRuntimeState::StartingTransport);
    assert(core.Handle(Event(CommissioningEventType::TransportStartFailed,
                             generation - 1)).empty());
    assert(core.state() == CommissioningRuntimeState::StartingTransport);
}

void TestCandidateCommitsOnlyAfterWifiAndOwnerValidation()
{
    CommissioningOrchestratorCore core;
    const uint32_t generation = OpenToSession(core);
    assert(core.Handle(Event(CommissioningEventType::NetworkCandidateReceived,
                             generation, "candidate-1")).empty());
    StageTrust(core, generation);
    auto actions = core.Handle(Event(
        CommissioningEventType::NetworkCandidateReceived, generation, "candidate-1"));
    assert(Has(actions, CommissioningActionType::StageNetworkCandidate));
    assert(!Has(actions, CommissioningActionType::CommitCommissioningTransaction));
    core.Handle(Event(CommissioningEventType::NetworkCandidateStaged,
                      generation, "candidate-1"));
    actions = core.Handle(Event(CommissioningEventType::WifiConnected,
                                generation, "candidate-1"));
    assert(Has(actions, CommissioningActionType::ValidateOwnerRoute));
    assert(!Has(actions, CommissioningActionType::CommitCommissioningTransaction));
    actions = core.Handle(Event(CommissioningEventType::OwnerRouteValidated,
                                generation, "candidate-1"));
    assert(Has(actions, CommissioningActionType::CommitCommissioningTransaction));
}

void TestTransportClosesOnlyAfterCommittedTerminalWasObserved()
{
    CommissioningOrchestratorCore core;
    const uint32_t generation = OpenToSession(core);
    StageTrust(core, generation);
    core.Handle(Event(CommissioningEventType::NetworkCandidateReceived,
                      generation, "candidate-1"));
    core.Handle(Event(CommissioningEventType::NetworkCandidateStaged,
                      generation, "candidate-1"));
    core.Handle(Event(CommissioningEventType::WifiConnected,
                      generation, "candidate-1"));
    core.Handle(Event(CommissioningEventType::OwnerRouteValidated,
                      generation, "candidate-1"));
    auto actions = core.Handle(Event(
        CommissioningEventType::CommissioningTransactionCommitted,
        generation, "candidate-1"));
    assert(core.transaction_committed());
    assert(!Has(actions, CommissioningActionType::StopTransport));
    actions = core.Handle(Event(
        CommissioningEventType::ControllerObservedTerminal, generation));
    assert(Has(actions, CommissioningActionType::StopTransport));
    assert(core.Handle(Event(
        CommissioningEventType::ControllerObservedTerminal, generation)).empty());

    actions = core.Handle(Event(CommissioningEventType::TransportStopped,
                                generation));
    assert(Has(actions, CommissioningActionType::RestorePreviousRadioMode));
    core.Handle(Event(CommissioningEventType::PreviousModeRestored,
                      generation));
    assert(core.state() == CommissioningRuntimeState::ReturningToPreviousMode);
    core.Handle(Event(CommissioningEventType::StationRouteReady, generation));
    assert(core.state() == CommissioningRuntimeState::Idle);
}

void TestPreparationFailureDoesNotWaitForTransportThatNeverExisted()
{
    CommissioningOrchestratorCore core;
    core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    const uint32_t generation = core.generation();
    auto actions = core.Handle(
        Event(CommissioningEventType::IdentityFailed, generation));
    assert(core.state() == CommissioningRuntimeState::Idle);
    assert(!Has(actions, CommissioningActionType::StopTransport));
    assert(!Has(actions, CommissioningActionType::RestorePreviousRadioMode));
}

void TestTrustRefusalReturnsBeforeTransportTeardown()
{
    CommissioningOrchestratorCore core;
    const uint32_t generation = OpenToSession(core);

    auto actions = core.Handle(
        Event(CommissioningEventType::TrustStageFailed, generation));
    assert(core.state() == CommissioningRuntimeState::SessionActive);
    assert(Has(actions, CommissioningActionType::PublishConfirmedState));
    assert(!Has(actions, CommissioningActionType::StopTransport));
    assert(!Has(actions,
                CommissioningActionType::RollbackCommissioningTransaction));

    actions = core.Handle(
        Event(CommissioningEventType::CancelRequested, generation));
    assert(Has(actions, CommissioningActionType::StopTransport));
}

void TestValidationFailureRollsBackBeforeTransportAndRadioRelease()
{
    CommissioningOrchestratorCore core;
    const uint32_t generation = OpenToSession(core);
    StageTrust(core, generation);
    core.Handle(Event(CommissioningEventType::NetworkCandidateReceived,
                      generation, "candidate-1"));
    core.Handle(Event(CommissioningEventType::NetworkCandidateStaged,
                      generation, "candidate-1"));
    auto actions = core.Handle(Event(
        CommissioningEventType::OwnerRouteValidationFailed, generation, "candidate-1"));
    assert(Has(actions, CommissioningActionType::RollbackCommissioningTransaction));
    assert(!Has(actions, CommissioningActionType::StopTransport));
    assert(core.Handle(Event(CommissioningEventType::WindowExpired,
                             generation)).empty());
    actions = core.Handle(Event(
        CommissioningEventType::CommissioningTransactionRolledBack,
        generation, "candidate-1"));
    assert(Has(actions, CommissioningActionType::StopTransport));
    actions = core.Handle(Event(CommissioningEventType::TransportStopped,
                                generation));
    assert(Has(actions, CommissioningActionType::RestorePreviousRadioMode));
    assert(core.Handle(Event(CommissioningEventType::TransportStopped,
                             generation)).empty());
    core.Handle(Event(CommissioningEventType::PreviousModeRestored, generation));
    assert(core.state() == CommissioningRuntimeState::Idle);
}

void TestStagedTrustRollsBackEvenBeforeNetworkArrives()
{
    CommissioningOrchestratorCore core;
    const uint32_t generation = OpenToSession(core);
    StageTrust(core, generation);
    auto actions = core.Handle(Event(CommissioningEventType::WindowExpired,
                                     generation));
    assert(Has(actions,
               CommissioningActionType::RollbackCommissioningTransaction));
    assert(!Has(actions, CommissioningActionType::StopTransport));
}

void TestUnexpectedTransportEndUsesTheSingleStopPath()
{
    CommissioningOrchestratorCore core;
    core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    const uint32_t generation = core.generation();
    core.Handle(Event(CommissioningEventType::IdentityReady, generation));
    core.Handle(Event(CommissioningEventType::OperationalRuntimeQuiesced,
                      generation));
    core.Handle(Event(CommissioningEventType::RadioAcquired, generation));

    auto actions = core.Handle(Event(
        CommissioningEventType::TransportEndedUnexpectedly, generation));
    assert(Has(actions, CommissioningActionType::StopTransport));
    assert(core.state() == CommissioningRuntimeState::RestoringPreviousMode);
    assert(core.Handle(Event(
        CommissioningEventType::TransportEndedUnexpectedly, generation)).empty());

    actions = core.Handle(Event(CommissioningEventType::TransportStopped,
                                generation));
    assert(Has(actions, CommissioningActionType::RestorePreviousRadioMode));
}

void TestRepeatedOpenRetainsOneGenerationAndOneStartAction()
{
    CommissioningOrchestratorCore core;
    auto actions = core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    assert(Has(actions, CommissioningActionType::EnsureIdentity));
    const uint32_t generation = core.generation();

    assert(core.Handle(Event(CommissioningEventType::OpenRequested, 0)).empty());
    assert(core.generation() == generation);
    core.Handle(Event(CommissioningEventType::IdentityReady, generation));
    core.Handle(Event(CommissioningEventType::OperationalRuntimeQuiesced,
                      generation));
    actions = core.Handle(Event(CommissioningEventType::RadioAcquired,
                                generation));
    assert(Has(actions, CommissioningActionType::StartTransport));
    assert(core.Handle(Event(CommissioningEventType::OpenRequested, 0)).empty());
    assert(core.generation() == generation);
}

void TestOperationalRuntimeMustQuiesceBeforeRadioLease()
{
    CommissioningOrchestratorCore core;
    core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    const uint32_t generation = core.generation();

    auto actions = core.Handle(
        Event(CommissioningEventType::IdentityReady, generation));
    assert(core.state() ==
           CommissioningRuntimeState::QuiescingOperationalRuntime);
    assert(Has(actions, CommissioningActionType::QuiesceOperationalRuntime));
    assert(!Has(actions,
                CommissioningActionType::AcquireCommissioningRadioLease));
    assert(core.Handle(Event(CommissioningEventType::RadioAcquired,
                             generation)).empty());

    actions = core.Handle(Event(
        CommissioningEventType::OperationalRuntimeQuiesced, generation));
    assert(core.state() == CommissioningRuntimeState::AcquiringRadio);
    assert(Has(actions,
               CommissioningActionType::AcquireCommissioningRadioLease));
}

void TestOperationalRuntimeQuiesceFailureNeverTouchesRadio()
{
    CommissioningOrchestratorCore core;
    core.Handle(Event(CommissioningEventType::OpenRequested, 0));
    const uint32_t generation = core.generation();
    core.Handle(Event(CommissioningEventType::IdentityReady, generation));

    const auto actions = core.Handle(Event(
        CommissioningEventType::OperationalRuntimeQuiesceFailed, generation));
    assert(core.state() == CommissioningRuntimeState::Idle);
    assert(!Has(actions,
                CommissioningActionType::AcquireCommissioningRadioLease));
    assert(!Has(actions, CommissioningActionType::StartTransport));
}


void TestDurableDecisionIgnoresCancelAndRetriesForward() {
    CommissioningOrchestratorCore core;
    const auto generation = OpenToSession(core);
    StageTrust(core, generation);
    for (const auto type : {CommissioningEventType::NetworkCandidateReceived,
                           CommissioningEventType::NetworkCandidateStaged,
                           CommissioningEventType::WifiConnected,
                           CommissioningEventType::OwnerRouteValidated,
                           CommissioningEventType::CommissioningTransactionRecoveryRequired}) {
        core.Handle(Event(type, generation, "candidate-1"));
    }
    assert(core.state() == CommissioningRuntimeState::RecoveringConfiguration);
    for (const auto type : {CommissioningEventType::CancelRequested,
                           CommissioningEventType::WindowExpired,
                           CommissioningEventType::TransportEndedUnexpectedly}) {
        assert(core.Handle(Event(type, generation)).empty());
    }
    const auto retry = core.Handle(Event(CommissioningEventType::RecoveryRetry, generation));
    assert(Has(retry, CommissioningActionType::RecoverCommissioningTransaction));
    const auto finished = core.Handle(Event(CommissioningEventType::CommissioningTransactionCommitted, generation, "candidate-1"));
    assert(core.transaction_committed());
    assert(Has(finished, CommissioningActionType::StopTransport));
    assert(!Has(finished, CommissioningActionType::RollbackCommissioningTransaction));
}


}  // namespace

int main()
{
    TestDurableDecisionIgnoresCancelAndRetriesForward();
    TestReadyRequiresEvidenceAndCallbacksAreGenerationFenced();
    TestCandidateCommitsOnlyAfterWifiAndOwnerValidation();
    TestTransportClosesOnlyAfterCommittedTerminalWasObserved();
    TestPreparationFailureDoesNotWaitForTransportThatNeverExisted();
    TestTrustRefusalReturnsBeforeTransportTeardown();
    TestValidationFailureRollsBackBeforeTransportAndRadioRelease();
    TestStagedTrustRollsBackEvenBeforeNetworkArrives();
    TestUnexpectedTransportEndUsesTheSingleStopPath();
    TestRepeatedOpenRetainsOneGenerationAndOneStartAction();
    TestOperationalRuntimeMustQuiesceBeforeRadioLease();
    TestOperationalRuntimeQuiesceFailureNeverTouchesRadio();
    return 0;
}
