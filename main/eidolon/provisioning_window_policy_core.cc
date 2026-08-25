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

int AdvertisedWindowSeconds(const ProvisioningWindowPolicy& policy)
{
    return policy.bounded ? policy.seconds : 0;
}

}  // namespace eidolon
