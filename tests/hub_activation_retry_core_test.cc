#include <cassert>

#include "device_state.h"
#include "eidolon/hub_activation_retry_core.h"
#include "eidolon/ui_state_mapper.h"

namespace {

using eidolon::ActivationAttemptOutcome;
using eidolon::ActivationStandDown;
using eidolon::EidolonRuntimeStatus;
using eidolon::HubActivationRetryCore;
using eidolon::UiStateProjector;

// The regression. A device whose Claim the Owner revoked, or whose Host is
// switched off, gets an error out of every attempt. It made exactly one before
// this, because the loop ended on "the device looks idle" and the screen the
// device shows while it waits is an idle-looking screen. Nothing about a failed
// attempt may end the loop.
void KeepsAskingForeverWhileOnlyAttemptsFail()
{
    HubActivationRetryCore retry;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        assert(retry.Evaluate(false) == ActivationStandDown::KeepAsking);
        assert(retry.OnAttempt(ActivationAttemptOutcome::Retryable) ==
               ActivationStandDown::KeepAsking);
        // Every second of every backoff, with nothing else happening.
        for (int second = 0; second < retry.delay_seconds(); ++second) {
            assert(retry.Evaluate(false) == ActivationStandDown::KeepAsking);
        }
    }
    assert(retry.retryable_attempts() == 5000);
    assert(retry.stand_down() == ActivationStandDown::KeepAsking);
}

// The interval grows and then stops growing, so a Host that comes back after an
// hour is found within two minutes rather than never.
void BackoffDoublesAndCapsAtTwoMinutes()
{
    HubActivationRetryCore retry;
    assert(retry.delay_seconds() == 10);

    const int expected[] = {10, 20, 40, 80, 120, 120, 120, 120};
    for (int i = 0; i < 8; ++i) {
        retry.OnAttempt(ActivationAttemptOutcome::Retryable);
        assert(retry.delay_seconds() == expected[i]);
    }
    assert(retry.delay_seconds() == HubActivationRetryCore::kMaxDelaySeconds);
}

// Commissioning owns the radio exclusively. It is the one thing that legitimately
// takes the device away from activation mid-wait.
void CommissioningEndsTheWaitOnTheSecondItStarts()
{
    HubActivationRetryCore retry;
    retry.OnAttempt(ActivationAttemptOutcome::Retryable);
    for (int second = 0; second < 4; ++second) {
        assert(retry.Evaluate(false) == ActivationStandDown::KeepAsking);
    }
    assert(retry.Evaluate(true) == ActivationStandDown::CommissioningOwnsRadio);
    // Sticky: the run is over even if commissioning finishes.
    assert(retry.Evaluate(false) == ActivationStandDown::CommissioningOwnsRadio);
    assert(retry.OnAttempt(ActivationAttemptOutcome::Admitted) ==
           ActivationStandDown::CommissioningOwnsRadio);
}

// A terminal Claim is the one failure that asking cannot fix. It has to end the
// loop with a reason, so the device can say what does fix it, rather than
// spinning behind a "looking for the Hub" screen forever.
void TerminalClaimStandsDownWithItsReason()
{
    HubActivationRetryCore retry;
    retry.OnAttempt(ActivationAttemptOutcome::Retryable);
    retry.OnAttempt(ActivationAttemptOutcome::Retryable);
    assert(retry.OnAttempt(ActivationAttemptOutcome::ClaimTerminal) ==
           ActivationStandDown::ClaimTerminal);
    assert(retry.Evaluate(false) == ActivationStandDown::ClaimTerminal);
}

void AdmissionEndsTheLoop()
{
    HubActivationRetryCore retry;
    retry.OnAttempt(ActivationAttemptOutcome::Retryable);
    assert(retry.OnAttempt(ActivationAttemptOutcome::Admitted) ==
           ActivationStandDown::Admitted);
}

// The other half of the same bug: the screen a device shows while it waits for
// approval must not be reported as the device state "idle".
void PendingApprovalDoesNotProjectIdleOverALifecycleState()
{
    EidolonRuntimeStatus status;
    status.runtime = eidolon::RuntimePhase::Normal;
    status.enrollment = eidolon::EnrollmentPhase::PendingReview;
    status.service = eidolon::ServicePhase::DiscoveringAuthority;
    status.conversation = eidolon::ConversationPhase::Closed;

    const DeviceState lifecycle[] = {
        kDeviceStateStarting, kDeviceStateWifiConfiguring, kDeviceStateActivating,
        kDeviceStateUpgrading, kDeviceStateAudioTesting,
    };
    for (DeviceState state : lifecycle) {
        assert(UiStateProjector::ProjectLegacyDeviceState(status, state) == state);
    }
    // Idle is left as it is too, so this is a no-op rather than a churn.
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateIdle) ==
           kDeviceStateIdle);
}

// It still owns the conversation, including ending one.
void ConversationStatesAreStillProjected()
{
    EidolonRuntimeStatus status;
    status.conversation = eidolon::ConversationPhase::Opening;
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateIdle) ==
           kDeviceStateConnecting);

    status.conversation = eidolon::ConversationPhase::Active;
    status.turn = eidolon::TurnPhase::AgentSpeaking;
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateConnecting) ==
           kDeviceStateSpeaking);

    status.turn = eidolon::TurnPhase::UserSpeaking;
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateSpeaking) ==
           kDeviceStateListening);

    // Ending a conversation returns the device to idle.
    status.conversation = eidolon::ConversationPhase::Ended;
    status.turn = eidolon::TurnPhase::Idle;
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateListening) ==
           kDeviceStateIdle);
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateSpeaking) ==
           kDeviceStateIdle);
    assert(UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateConnecting) ==
           kDeviceStateIdle);
}

}  // namespace

int main()
{
    KeepsAskingForeverWhileOnlyAttemptsFail();
    BackoffDoublesAndCapsAtTwoMinutes();
    CommissioningEndsTheWaitOnTheSecondItStarts();
    TerminalClaimStandsDownWithItsReason();
    AdmissionEndsTheLoop();
    PendingApprovalDoesNotProjectIdleOverALifecycleState();
    ConversationStatesAreStillProjected();
    return 0;
}
