#include <cassert>
#include <optional>
#include <string>

#include "eidolon/provisioning_window_policy_core.h"

namespace {

using namespace eidolon;

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

void AuthorizedWindowsRemainAvailableWithoutAnAdvertisedDeadline()
{
    for (const auto trigger : {ProvisioningWindowTrigger::NeverCommissioned,
                               ProvisioningWindowTrigger::OwnerPresenceReopen}) {
        const auto policy = DecideProvisioningWindow(trigger);
        assert(!policy.bounded);
        assert(policy.seconds == 0);
        assert(!AdvertisedWindowSeconds(policy).has_value());
    }
    // An indefinite manual window must not enable automatic reopening.
    assert(AutomaticSetupOpenIsForbidden(ProvisioningWindowTrigger::OwnerPresenceReopen));
}

void ExplicitBoundedWireDurationsStillRequirePositiveSeconds()
{
    assert(AdvertisedWindowSeconds({true, 60})->seconds() == 60);
    assert(!AdvertisedWindowSeconds({true, 0}).has_value());
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

void LongPressSetupIsLimitedToNonConversationStates()
{
    const DeviceState allowed_states[] = {
        kDeviceStateIdle, kDeviceStateStarting, kDeviceStateActivating,
        kDeviceStateWifiConfiguring, kDeviceStateConnecting};
    for (const auto state : allowed_states) {
        assert(HubSetupButtonLongPressOpensSetup(state));
    }
    const DeviceState rejected_states[] = {
        kDeviceStateUnknown, kDeviceStateListening, kDeviceStateSpeaking,
        kDeviceStateUpgrading, kDeviceStateAudioTesting, kDeviceStateFatalError};
    for (const auto state : rejected_states) {
        assert(!HubSetupButtonLongPressOpensSetup(state));
    }
}

// An OTA that is half written is the one thing a setup window must not
// interrupt, and a device that has not finished deciding what it is has nothing
// to hand over yet.
void SetupStaysShutWhereOpeningItWouldBreakSomething()
{
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUpgrading));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateUnknown));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateConnecting));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateAudioTesting));
    assert(!HubDeviceStateAllowsSetupOpen(kDeviceStateFatalError));
}

}  // namespace

int main()
{
    AnEmptyTrustStoreMeansThisDeviceBelongsToNobodyYet();
    ADeviceNobodyHasClaimedYetStillOpensItsOwnWindow();
    ACommissionedDeviceMayNotOpenAWindowBecauseItsNetworkWentAway();
    AuthorizedWindowsRemainAvailableWithoutAnAdvertisedDeadline();
    ExplicitBoundedWireDurationsStillRequirePositiveSeconds();
    SetupOpensFromEveryStateAFailedActivationCanLeaveTheDeviceIn();
    ShortClickKeepsItsSetupMeaningAsBootAdvances();
    SetupOpensFromAWorkingDevice();
    LongPressSetupIsLimitedToNonConversationStates();
    SetupStaysShutWhereOpeningItWouldBreakSomething();
    return 0;
}
