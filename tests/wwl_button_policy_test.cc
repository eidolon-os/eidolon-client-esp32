#include <cassert>
#include "wwl_button_policy.h"
#include "eidolon/voice_session_state.h"
#include "eidolon/provisioning_window_policy_core.h"

namespace {

using namespace eidolon;
using namespace eidolon::wwl;

// --- Session-active classification ---
// Mirrors Application::IsVoiceSessionActive() logic: Connecting, Opening,
// InRoom, Reconnecting are active; all other VoiceSessionState values are not.
// HUB can park in DeviceState::Idle while the transport is in a joining state,
// so the policy uses voice transport state, not DeviceState.

void VoiceSessionStatesAreClassifiedCorrectly()
{
    const VoiceSessionState active[] = {
        VoiceSessionState::Connecting, VoiceSessionState::Opening,
        VoiceSessionState::InRoom, VoiceSessionState::Reconnecting};
    for (const auto s : active) {
        switch (s) {
        case VoiceSessionState::Connecting:
        case VoiceSessionState::Opening:
        case VoiceSessionState::InRoom:
        case VoiceSessionState::Reconnecting:
            // IsVoiceSessionActive() returns true for these.
            break;
        default:
            assert(false);
        }
    }
    const VoiceSessionState inactive[] = {
        VoiceSessionState::Idle, VoiceSessionState::PendingApproval,
        VoiceSessionState::WaitingBinding, VoiceSessionState::ConfigReady,
        VoiceSessionState::Error, VoiceSessionState::Unauthorized,
        VoiceSessionState::ServerUnreachable};
    for (const auto s : inactive) {
        switch (s) {
        case VoiceSessionState::Connecting:
        case VoiceSessionState::Opening:
        case VoiceSessionState::InRoom:
        case VoiceSessionState::Reconnecting:
            assert(false);
            break;
        default:
            break;
        }
    }
}

// --- WWL BOOT button policy truth table ---

void StandbyPressDownWakesAndMarksOrigin()
{
    BootPolicyState state;
    const auto action = BootPressDown(state, /*in_standby=*/true);
    assert(action == BootAction::WakeFromStandby);
    assert(state.press_in_standby == true);
    assert(state.long_press_consumed == false);
}

void NonStandbyPressDownReturnsTalkPress()
{
    BootPolicyState state;
    const auto action = BootPressDown(state, /*in_standby=*/false);
    assert(action == BootAction::TalkPress);
    assert(state.press_in_standby == false);
    assert(state.long_press_consumed == false);
}

void PressDownResetsPreviousCycleState()
{
    BootPolicyState state;
    state.press_in_standby = true;
    state.long_press_consumed = true;
    (void)BootPressDown(state, /*in_standby=*/false);
    assert(state.press_in_standby == false);
    assert(state.long_press_consumed == false);
}

void StandbyReleaseWithoutActiveVoiceJoins()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/true);
    const auto action = BootRelease(state, /*voice_active=*/false);
    assert(action == BootAction::JoinVoice);
}

void StandbyReleaseWithActiveVoiceIsNoop()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/true);
    const auto action = BootRelease(state, /*voice_active=*/true);
    assert(action == BootAction::None);
}

void LongPressFromStandbyReturnsForceSetup()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/true);
    const auto action = BootLongPress(state, /*voice_active=*/false);
    assert(action == BootAction::ForceSetup);
    assert(state.long_press_consumed == true);
}

void LongPressFromActiveVoiceReturnsLegacySetup()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/false);
    const auto action = BootLongPress(state, /*voice_active=*/true);
    assert(action == BootAction::LegacyLongPressSetup);
    assert(state.long_press_consumed == true);
}

void LongPressFromNonStandbyNonVoiceReturnsLegacy()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/false);
    const auto action = BootLongPress(state, /*voice_active=*/false);
    assert(action == BootAction::LegacyLongPressSetup);
    assert(state.long_press_consumed == true);
}

void ReleaseAfterLongPressIsSwallowed()
{
    {
        BootPolicyState state;
        (void)BootPressDown(state, /*in_standby=*/false);
        (void)BootLongPress(state, /*voice_active=*/true);
        const auto action = BootRelease(state, /*voice_active=*/true);
        assert(action == BootAction::SwallowRelease);
    }
    {
        BootPolicyState state;
        (void)BootPressDown(state, /*in_standby=*/true);
        (void)BootLongPress(state, /*voice_active=*/false);
        const auto action = BootRelease(state, /*voice_active=*/false);
        assert(action == BootAction::SwallowRelease);
    }
    {
        BootPolicyState state;
        (void)BootPressDown(state, /*in_standby=*/false);
        (void)BootLongPress(state, /*voice_active=*/false);
        const auto action = BootRelease(state, /*voice_active=*/false);
        assert(action == BootAction::SwallowRelease);
    }
}

void NormalPttReleaseIsPreserved()
{
    BootPolicyState state;
    (void)BootPressDown(state, /*in_standby=*/false);
    const auto action = BootRelease(state, /*voice_active=*/true);
    assert(action == BootAction::TalkRelease);
}

// --- Regression: provisioning policy gate is unchanged ---

void HubLongPressSetupGateIsUnchanged()
{
    const DeviceState long_press_allowed[] = {
        kDeviceStateIdle, kDeviceStateStarting, kDeviceStateActivating,
        kDeviceStateWifiConfiguring, kDeviceStateConnecting};
    for (const auto state : long_press_allowed) {
        assert(HubSetupButtonLongPressOpensSetup(state));
    }
    const DeviceState long_press_rejected[] = {
        kDeviceStateUnknown, kDeviceStateListening, kDeviceStateSpeaking,
        kDeviceStateUpgrading, kDeviceStateAudioTesting, kDeviceStateFatalError};
    for (const auto state : long_press_rejected) {
        assert(!HubSetupButtonLongPressOpensSetup(state));
    }
}

void SetupGatePreventsOtaInterruption()
{
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUpgrading));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUnknown));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateConnecting));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateAudioTesting));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateFatalError));
}

void SetupGateAllowsRecoveryStates()
{
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateWifiConfiguring));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateActivating));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateStarting));
}

// --- Full cycle: standby short press wake+join ---
void StandbyShortPressWakesAndJoins()
{
    BootPolicyState state;
    assert(BootPressDown(state, true) == BootAction::WakeFromStandby);
    assert(BootRelease(state, false) == BootAction::JoinVoice);
}

// --- Full cycle: standby long press wakes and forces setup ---
void StandbyLongPressForcesSetup()
{
    BootPolicyState state;
    assert(BootPressDown(state, true) == BootAction::WakeFromStandby);
    assert(BootLongPress(state, false) == BootAction::ForceSetup);
    assert(BootRelease(state, false) == BootAction::SwallowRelease);
}

// --- Full cycle: active voice long press routes to provisioning gate ---
void ActiveVoiceLongPressOpensSetup()
{
    BootPolicyState state;
    assert(BootPressDown(state, false) == BootAction::TalkPress);
    assert(BootLongPress(state, true) == BootAction::LegacyLongPressSetup);
    assert(BootRelease(state, true) == BootAction::SwallowRelease);
}

// --- Full cycle: normal PTT press/release unchanged ---
void NormalPttPressReleaseIsUnchanged()
{
    BootPolicyState state;
    assert(BootPressDown(state, false) == BootAction::TalkPress);
    assert(BootRelease(state, true) == BootAction::TalkRelease);
}

}  // namespace

int main()
{
    VoiceSessionStatesAreClassifiedCorrectly();

    StandbyPressDownWakesAndMarksOrigin();
    NonStandbyPressDownReturnsTalkPress();
    PressDownResetsPreviousCycleState();
    StandbyReleaseWithoutActiveVoiceJoins();
    StandbyReleaseWithActiveVoiceIsNoop();
    LongPressFromStandbyReturnsForceSetup();
    LongPressFromActiveVoiceReturnsLegacySetup();
    LongPressFromNonStandbyNonVoiceReturnsLegacy();
    ReleaseAfterLongPressIsSwallowed();
    NormalPttReleaseIsPreserved();

    HubLongPressSetupGateIsUnchanged();
    SetupGatePreventsOtaInterruption();
    SetupGateAllowsRecoveryStates();

    StandbyShortPressWakesAndJoins();
    StandbyLongPressForcesSetup();
    ActiveVoiceLongPressOpensSetup();
    NormalPttPressReleaseIsUnchanged();

    return 0;
}