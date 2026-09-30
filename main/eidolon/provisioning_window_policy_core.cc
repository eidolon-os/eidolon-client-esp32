#include "provisioning_window_policy_core.h"

namespace eidolon {

ProvisioningWindowTrigger ProvisioningWindowTriggerFor(
    const std::string& commissioned_owner_domain_id)
{
    return commissioned_owner_domain_id.empty()
               ? ProvisioningWindowTrigger::NeverCommissioned
               : ProvisioningWindowTrigger::OwnerPresenceReopen;
}

bool AutomaticSetupOpenIsForbidden(ProvisioningWindowTrigger trigger)
{
    switch (trigger) {
    case ProvisioningWindowTrigger::NeverCommissioned:
        // The out-of-the-box path. There is no Owner to protect and no other
        // way in, so this window is the product working.
        return false;
    case ProvisioningWindowTrigger::OwnerPresenceReopen:
        // D8. The network is what went away; the Owner did not.
        return true;
    }
    // A trigger this core does not name is not evidence of a device with
    // nothing to protect, and the two mistakes do not cost the same: refusing
    // costs a button press, opening offers the device to whoever is nearby.
    return true;
}

ProvisioningWindowPolicy DecideProvisioningWindow(ProvisioningWindowTrigger)
{
    return {false, 0};
}

std::optional<device_foundation::v1::SetupWindowRemainingSeconds>
AdvertisedWindowSeconds(const ProvisioningWindowPolicy& policy)
{
    if (!policy.bounded) {
        return std::nullopt;
    }
    // Retain support for positive bounded durations in the wire contract.
    return device_foundation::v1::SetupWindowRemainingSeconds::FromPositiveSeconds(
        policy.seconds);
}

bool HubSetupButtonClickOpensSetup(DeviceState state)
{
    return state == kDeviceStateStarting ||
           state == kDeviceStateActivating ||
           state == kDeviceStateWifiConfiguring;
}

bool HubSetupButtonLongPressOpensSetup(DeviceState state)
{
    // Same set as HubDeviceStateAllowsSetupOpen, except the long press also opens
    // from Connecting (a device still joining may need to be reclaimed) and never
    // opens from Listening or Speaking, whose button must stay exclusively on PTT.
    if (state == kDeviceStateConnecting) {
        return true;
    }
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        return false;
    }
    return HubDeviceStateAllowsSetupOpen(state);
}

bool HubDeviceStateAllowsSetupOpen(DeviceState state)
{
    switch (state) {
    case kDeviceStateIdle:
    case kDeviceStateListening:
    case kDeviceStateSpeaking:
    case kDeviceStateStarting:
    case kDeviceStateActivating:
    case kDeviceStateWifiConfiguring:
        return true;
    case kDeviceStateUnknown:
    case kDeviceStateConnecting:
    case kDeviceStateUpgrading:
    case kDeviceStateAudioTesting:
    case kDeviceStateFatalError:
        return false;
    }
    return false;
}

}  // namespace eidolon
