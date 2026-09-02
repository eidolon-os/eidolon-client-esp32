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

ProvisioningWindowPolicy DecideProvisioningWindow(
    ProvisioningWindowTrigger trigger, int configured_window_seconds)
{
    ProvisioningWindowPolicy policy;
    if (trigger == ProvisioningWindowTrigger::NeverCommissioned) {
        policy.bounded = false;
        policy.seconds = 0;
        return policy;
    }
    policy.bounded = true;
    // A build whose configuration escaped the Kconfig range must not turn a
    // bounded window into a window that closes the instant it opens: that
    // reads to the Owner as a device that never came up, and to the controller
    // as a device that vanished mid-setup.
    policy.seconds = configured_window_seconds;
    if (policy.seconds < ProvisioningWindowBounds::kMinSeconds) {
        policy.seconds = ProvisioningWindowBounds::kMinSeconds;
    } else if (policy.seconds > ProvisioningWindowBounds::kMaxSeconds) {
        policy.seconds = ProvisioningWindowBounds::kMaxSeconds;
    }
    return policy;
}

std::optional<device_foundation::v1::SetupWindowRemainingSeconds>
AdvertisedWindowSeconds(const ProvisioningWindowPolicy& policy)
{
    if (!policy.bounded) {
        return std::nullopt;
    }
    // DecideProvisioningWindow has already clamped a bounded window into the
    // configured range, so this can only fail if that stopped being true.
    return device_foundation::v1::SetupWindowRemainingSeconds::FromPositiveSeconds(
        policy.seconds);
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
