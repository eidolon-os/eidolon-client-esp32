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
    assert(AdvertisedWindowSeconds(DecideProvisioningWindow(
               ProvisioningWindowTrigger::OwnerPresenceReopen, 600)) == 600);
}

void AnUnboundedWindowHasNoDurationToAdvertiseAtAll()
{
    // Not "zero seconds" — no number. A factory device once advertised 0 here
    // and the controller, which requires a positive duration, refused every
    // brand new device on the grounds that its descriptor broke the contract.
    // "There is a deadline" and "there is no deadline" are two different facts,
    // so the absent one must not be reachable as an int.
    const std::optional<int> advertised = AdvertisedWindowSeconds(
        DecideProvisioningWindow(ProvisioningWindowTrigger::NeverCommissioned, 600));
    assert(!advertised.has_value());
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
    return 0;
}
