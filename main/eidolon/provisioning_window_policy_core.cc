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

bool HubDeviceStateAllowsSetupOpen(DeviceState state)
{
    return HubSetupRefusalNotice(state) == nullptr;
}

const char* HubSetupRefusalNotice(DeviceState state)
{
    switch (state) {
    case kDeviceStateIdle:
    case kDeviceStateConnecting:
    case kDeviceStateListening:
    case kDeviceStateSpeaking:
    case kDeviceStateStarting:
    case kDeviceStateActivating:
    case kDeviceStateWifiConfiguring:
        return nullptr;
    case kDeviceStateUpgrading:
        return "Setup cannot open during an update. Try again when it finishes";
    case kDeviceStateUnknown:
        return "Still starting. Try again in a moment";
    case kDeviceStateAudioTesting:
        return "Audio test running. Try again when it ends";
    case kDeviceStateFatalError:
        break;
    }
    // A fault, or a state this core does not name: refused, and a restart is
    // the way back to one that is not.
    return "This device stopped on a fault. Restart it, then try again";
}

}  // namespace eidolon
