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

void AFactoryDeviceKeepsOfferingItselfUntilSomebodyClaimsIt()
{
    const ProvisioningWindowPolicy policy = DecideProvisioningWindow(
        ProvisioningWindowTrigger::NeverCommissioned, 600);
    assert(!policy.bounded);
    assert(policy.seconds == 0);
}

void AnUnboundedWindowIgnoresTheConfiguredDurationEntirely()
{
    for (const int configured : {0, 60, 600, 3600, 99999}) {
        const ProvisioningWindowPolicy policy = DecideProvisioningWindow(
            ProvisioningWindowTrigger::NeverCommissioned, configured);
        assert(!policy.bounded);
        assert(policy.seconds == 0);
    }
}

void AnOwnerReopeningSetupGetsTheConfiguredBoundedWindow()
{
    const ProvisioningWindowPolicy policy = DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen, 600);
    assert(policy.bounded);
    assert(policy.seconds == 600);
}

void TheConfiguredRangeEndpointsPassThroughUntouched()
{
    const ProvisioningWindowPolicy shortest = DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen,
        ProvisioningWindowBounds::kMinSeconds);
    assert(shortest.bounded);
    assert(shortest.seconds == 60);

    const ProvisioningWindowPolicy longest = DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen,
        ProvisioningWindowBounds::kMaxSeconds);
    assert(longest.bounded);
    assert(longest.seconds == 3600);
}

void ADurationOutsideTheConfiguredRangeStillLeavesAUsableWindow()
{
    // A window of zero or a negative one would close the moment it opened,
    // which on the Owner's side is indistinguishable from a device that never
    // came up at all — the failure this whole policy exists to prevent.
    for (const int configured : {-3600, -1, 0, 1, 59}) {
        const ProvisioningWindowPolicy policy = DecideProvisioningWindow(
            ProvisioningWindowTrigger::OwnerPresenceReopen, configured);
        assert(policy.bounded);
        assert(policy.seconds == ProvisioningWindowBounds::kMinSeconds);
    }
    const ProvisioningWindowPolicy policy = DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen, 86400);
    assert(policy.bounded);
    assert(policy.seconds == ProvisioningWindowBounds::kMaxSeconds);
}

void TheAdvertisedDurationSaysWhatTheDeviceWillActuallyDo()
{
    // The controller computes its own expiry from this number, so an unbounded
    // offer must not advertise one: a device that announced 600 seconds and
    // then kept listening would be lying in the direction that makes the
    // controller give up while the device is still reachable.
    const auto advertised = AdvertisedWindowSeconds(DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen, 600));
    assert(advertised.has_value());
    assert(advertised->seconds() == 600);
}

void AnUnboundedWindowHasNoDurationToAdvertiseAtAll()
{
    // Not "zero seconds" — no number. A factory device once advertised 0 here
    // and the controller, which requires a positive duration, refused every
    // brand new device on the grounds that its descriptor broke the contract.
    // "There is a deadline" and "there is no deadline" are two different facts,
    // so the absent one must not be reachable as a number — the canonical
    // duration type has no representation for one, and this returns nothing.
    const auto advertised = AdvertisedWindowSeconds(
        DecideProvisioningWindow(ProvisioningWindowTrigger::NeverCommissioned, 600));
    assert(!advertised.has_value());
    assert(!device_foundation::v1::SetupWindowRemainingSeconds::FromPositiveSeconds(0)
                .has_value());
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

void SetupOpensFromAWorkingDevice()
{
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateIdle));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateListening));
    assert(HubDeviceStateAllowsSetupOpen(kDeviceStateSpeaking));
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
    AFactoryDeviceKeepsOfferingItselfUntilSomebodyClaimsIt();
    AnUnboundedWindowIgnoresTheConfiguredDurationEntirely();
    AnOwnerReopeningSetupGetsTheConfiguredBoundedWindow();
    TheConfiguredRangeEndpointsPassThroughUntouched();
    ADurationOutsideTheConfiguredRangeStillLeavesAUsableWindow();
    TheAdvertisedDurationSaysWhatTheDeviceWillActuallyDo();
    AnUnboundedWindowHasNoDurationToAdvertiseAtAll();
    SetupOpensFromEveryStateAFailedActivationCanLeaveTheDeviceIn();
    SetupOpensFromAWorkingDevice();
    SetupStaysShutWhereOpeningItWouldBreakSomething();
    return 0;
}
