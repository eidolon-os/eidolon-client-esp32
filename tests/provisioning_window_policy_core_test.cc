#include <cassert>
#include <cstring>
#include <string>

#include "eidolon/provisioning_window_policy_core.h"
#include "eidolon/ui_state_mapper.h"

namespace {

using namespace eidolon;

constexpr DeviceState kEveryDeviceState[] = {
    kDeviceStateUnknown,     kDeviceStateStarting,  kDeviceStateWifiConfiguring,
    kDeviceStateIdle,        kDeviceStateConnecting, kDeviceStateListening,
    kDeviceStateSpeaking,    kDeviceStateUpgrading, kDeviceStateActivating,
    kDeviceStateAudioTesting, kDeviceStateFatalError};

void AnEmptyTrustStoreMeansThisDeviceBelongsToNobodyYet()
{
    assert(ProvisioningWindowTriggerFor("") ==
           ProvisioningWindowTrigger::NeverCommissioned);
    assert(ProvisioningWindowTriggerFor("owner-domain-7f3a") ==
           ProvisioningWindowTrigger::OwnerPresenceReopen);
}

// Forbidden path D8: 连不上网络后自动开设置窗口 → 只进入 NetworkRecoveryRequired；
// Opening still requires physical presence or an authenticated administrator. A boot
// that found no network profile, and sixty seconds of Station failing — ask this
// before they ask for a window.

void ADeviceNobodyHasClaimedYetStillOpensItsOwnWindow()
{
    // The out-of-the-box path, and the reason this is not simply "never open a
    // window automatically": refusing here would leave a new board with no way
    // to be set up at all.
    assert(!AutomaticSetupOpenIsForbidden(
        ProvisioningWindowTrigger::NeverCommissioned));
}

void ACommissionedDeviceMayNotOpenAWindowBecauseItsNetworkWentAway()
{
    // A router that rebooted, a Wi-Fi password somebody changed, or anyone able
    // to take the network away for a minute is not this device's Owner asking
    // for anything. A bounded offer is still an offer, and whoever caused the
    // outage chooses when it opens.
    assert(AutomaticSetupOpenIsForbidden(
        ProvisioningWindowTrigger::OwnerPresenceReopen));
}

// The code is shown, and the gesture that opened setup closes it, only while a
// phone is being offered the window: closing one mid-apply would roll back what
// the phone is about to be told is done, and a window still starting or
// already closing has nothing to close.
void OnlyAWindowOfferedToAPhoneShowsItsCodeOrCloses()
{
    using State = CommissioningRuntimeState;
    for (const auto state : {State::Advertising, State::SessionActive}) {
        assert(SetupWindowIsOffered(state));
    }
    for (const auto state :
         {State::Idle, State::PreparingIdentity, State::QuiescingOperationalRuntime,
          State::AcquiringRadio, State::StartingTransport,
          State::ApplyingConfiguration, State::RecoveringConfiguration,
          State::ReturningToPreviousMode, State::RestoringPreviousMode}) {
        assert(!SetupWindowIsOffered(state));
    }
}

// The state a removed device is parked in must be able to open setup, or the
// screen telling its Owner to claim it again is asking for something the device
// refuses to do — and the only remaining way back erases the identity that made
// it the same device.
void SetupOpensFromEveryStateAFailedActivationCanLeaveTheDeviceIn()
{
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateWifiConfiguring));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateActivating));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateStarting));
}

void ShortClickKeepsItsSetupMeaningAsBootAdvances()
{
    const DeviceState startup_and_recovery[] = {
        kDeviceStateStarting, kDeviceStateActivating, kDeviceStateWifiConfiguring};
    for (const auto state : startup_and_recovery) {
        assert(HubSetupButtonClickOpensSetup(state));
        assert(HubDeviceStateAllowsSetupOpen(state));
    }
    // Reaching voice operation must retain the ordinary chat gesture, and
    // clicking during OTA must not request a new commissioning generation.
    const DeviceState other_states[] = {
        kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
        kDeviceStateConnecting, kDeviceStateUpgrading, kDeviceStateUnknown,
        kDeviceStateAudioTesting, kDeviceStateFatalError};
    for (const auto state : other_states) {
        assert(!HubSetupButtonClickOpensSetup(state));
    }
}

void SetupOpensFromAWorkingDevice()
{
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateIdle));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateListening));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateSpeaking));
}

// In HUB_MODE "connecting" is not the sub-second audio-channel open it was
// upstream. The presenter projects every service phase short of Ready onto it,
// so a device sits there for as long as its Host is gone, and that is exactly
// when the person in front of it needs to hand it a different network or Host.
void SetupOpensWhileTheServiceIsStillConnecting()
{
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateConnecting));
}

// An OTA that is half written is the one thing a setup window must not
// interrupt, and a device that has not finished deciding what it is has nothing
// to hand over yet.
void SetupStaysShutWhereOpeningItWouldBreakSomething()
{
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUpgrading));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUnknown));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateAudioTesting));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateFatalError));
}

// The press was the person asking. Turning it away with nothing on screen
// leaves them holding a button that looks broken; the notice says what is in
// the way and that trying again later will work.
void ARefusedPressSaysWhyOnScreen()
{
    for (const auto state : kEveryDeviceState) {
        const char* notice = HubSetupRefusalNotice(state);
        if (HubDeviceStateAllowsSetupOpen(state)) {
            assert(notice == nullptr);
        } else {
            assert(notice != nullptr && std::strlen(notice) > 0);
        }
    }
    assert(std::strstr(HubSetupRefusalNotice(kDeviceStateUpgrading), "update") != nullptr);
}

EidolonRuntimeStatus OperationalStatus()
{
    EidolonRuntimeStatus status;
    status.runtime = RuntimePhase::Normal;
    status.enrollment = EnrollmentPhase::ClaimActive;
    status.service = ServicePhase::Ready;
    return status;
}

// What the presenter writes into DeviceState for a status, starting from a
// device that was idle — the press arrives after the projection has settled.
DeviceState SettledLegacyState(const EidolonRuntimeStatus& status)
{
    return UiStateProjector::ProjectLegacyDeviceState(status, kDeviceStateIdle);
}

// Each of these is a screen that offers setup — on BOX-3 by naming the long
// press, on a touch panel as a setup button — and each one used to be drawn
// over a DeviceState the gesture refused, so the offer did nothing at all.
void EveryScreenThatOffersSetupCanOpenIt()
{
    // Wi-Fi went away under an idle device. The voice controller's network_lost
    // projects service Reconnecting, and the screen says "Looking for saved
    // Wi-Fi. Hold BOOT to change network".
    auto wifi_lost = OperationalStatus();
    wifi_lost.runtime = RuntimePhase::NetworkScanning;
    wifi_lost.runtime_detail = "Looking for saved Wi-Fi. Hold BOOT to change network";
    wifi_lost.service = ServicePhase::Reconnecting;
    assert(UiStateProjector::AllowsIntent(wifi_lost, UiIntent::OpenSetup));
    assert(SettledLegacyState(wifi_lost) == kDeviceStateConnecting);
    assert(HubDeviceStateAllowsSetupOpen(SettledLegacyState(wifi_lost)));

    // The Host restarted, moved or was reinstalled: Wi-Fi is fine, the
    // operational channel is not, for as long as the Host stays away.
    auto host_gone = OperationalStatus();
    host_gone.service = ServicePhase::Reconnecting;
    assert(UiStateProjector::AllowsIntent(host_gone, UiIntent::OpenSetup));
    assert(HubDeviceStateAllowsSetupOpen(SettledLegacyState(host_gone)));

    // The first control-room join after boot.
    auto first_join = OperationalStatus();
    first_join.service = ServicePhase::Connecting;
    assert(HubDeviceStateAllowsSetupOpen(SettledLegacyState(first_join)));

    // A conversation that failed is also projected as connecting.
    auto failed = OperationalStatus();
    failed.service = ServicePhase::Fault;
    failed.conversation = ConversationPhase::Failed;
    assert(UiStateProjector::AllowsIntent(failed, UiIntent::OpenSetup));
    assert(HubDeviceStateAllowsSetupOpen(SettledLegacyState(failed)));
}

// The same rule over every combination the projector can be handed: wherever it
// lets setup be asked for, whatever the presenter makes of DeviceState from a
// resting state is one the gesture accepts.
void NoProjectedStateOffersSetupThatTheGestureRefuses()
{
    const RuntimePhase runtimes[] = {
        RuntimePhase::Booting, RuntimePhase::LoadingAssets,
        RuntimePhase::NetworkScanning, RuntimePhase::NetworkConnecting,
        RuntimePhase::Commissioning, RuntimePhase::Normal, RuntimePhase::Updating,
        RuntimePhase::RecoveryRequired, RuntimePhase::Fault};
    const ServicePhase services[] = {
        ServicePhase::Unavailable, ServicePhase::DiscoveringAuthority,
        ServicePhase::Registering, ServicePhase::Preparing, ServicePhase::Ready,
        ServicePhase::Connecting, ServicePhase::Reconnecting,
        ServicePhase::Unreachable, ServicePhase::Fault};
    const ConversationPhase conversations[] = {
        ConversationPhase::Closed, ConversationPhase::Opening,
        ConversationPhase::Active, ConversationPhase::Reconnecting,
        ConversationPhase::Ended, ConversationPhase::Failed};
    // Upgrading is the Application's to enter and leave; nothing projects it.
    const DeviceState resting[] = {
        kDeviceStateStarting, kDeviceStateActivating, kDeviceStateWifiConfiguring,
        kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening,
        kDeviceStateSpeaking};
    for (const auto runtime : runtimes) {
        for (const auto service : services) {
            for (const auto conversation : conversations) {
                EidolonRuntimeStatus status = OperationalStatus();
                status.runtime = runtime;
                status.service = service;
                status.conversation = conversation;
                if (!UiStateProjector::AllowsIntent(status, UiIntent::OpenSetup)) continue;
                for (const auto current : resting) {
                    assert(HubDeviceStateAllowsSetupOpen(
                        UiStateProjector::ProjectLegacyDeviceState(status, current)));
                }
            }
        }
    }
}

}  // namespace

int main()
{
    AnEmptyTrustStoreMeansThisDeviceBelongsToNobodyYet();
    ADeviceNobodyHasClaimedYetStillOpensItsOwnWindow();
    ACommissionedDeviceMayNotOpenAWindowBecauseItsNetworkWentAway();
    OnlyAWindowOfferedToAPhoneShowsItsCodeOrCloses();
    SetupOpensFromEveryStateAFailedActivationCanLeaveTheDeviceIn();
    ShortClickKeepsItsSetupMeaningAsBootAdvances();
    SetupOpensFromAWorkingDevice();
    SetupOpensWhileTheServiceIsStillConnecting();
    SetupStaysShutWhereOpeningItWouldBreakSomething();
    ARefusedPressSaysWhyOnScreen();
    EveryScreenThatOffersSetupCanOpenIt();
    NoProjectedStateOffersSetupThatTheGestureRefuses();
    return 0;
}
