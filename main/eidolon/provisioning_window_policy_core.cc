#include "provisioning_window_policy_core.h"

namespace eidolon {

ProvisioningWindowTrigger ProvisioningWindowTriggerFor(
    const std::string& commissioned_owner_domain_id)
{
    return commissioned_owner_domain_id.empty()
               ? ProvisioningWindowTrigger::NeverCommissioned
               : ProvisioningWindowTrigger::OwnerPresenceReopen;
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

}  // namespace eidolon
