#ifndef EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
#define EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_

#include <optional>
#include <string>

namespace eidolon {

// Why this device is offering to be set up right now. The two cases are not
// variations of one act: they differ in what the device already holds, and
// therefore in what an open setup offer can cost.
enum class ProvisioningWindowTrigger {
    // This device holds no commissioned Owner Domain: factory state, or an
    // Owner erase that returned it there. It has no trust material, no Claim
    // and no network to lose, and nobody has yet told it who it belongs to.
    NeverCommissioned,
    // A commissioned device reopened setup on the physical-presence gesture.
    // It holds Owner trust material, so the open offer is the one thing that
    // could hand it to somebody else.
    OwnerPresenceReopen,
};

struct ProvisioningWindowPolicy {
    // False means the device keeps advertising until it is claimed, cancelled
    // or powered off. True means it closes itself after `seconds`.
    bool bounded = true;
    // Only meaningful when bounded.
    int seconds = 0;
};

// Mirrors the range on EIDOLON_PROVISIONING_WINDOW_SECONDS. Kept here so this
// core can be reasoned about without Kconfig; device_provisioning.cc asserts
// the two cannot drift apart.
struct ProvisioningWindowBounds {
    static constexpr int kMinSeconds = 60;
    static constexpr int kMaxSeconds = 3600;
};

// The trust store is the authority on whether this device has an Owner: an
// empty commissioned Owner Domain id is the only evidence of a device that
// belongs to nobody.
ProvisioningWindowTrigger ProvisioningWindowTriggerFor(
    const std::string& commissioned_owner_domain_id);

// A bounded window protects a commissioned device from being taken over by
// whoever is nearby, and the physical gesture that opened it is the Owner
// saying so. An uncommissioned device has nothing to take over — but a device
// that closes its only setup offer and cannot reopen it without somebody
// physically holding a button is, from the Owner's side, indistinguishable from
// broken hardware. So the window is bounded exactly when there is something to
// protect, and never on a device still waiting to be claimed.
ProvisioningWindowPolicy DecideProvisioningWindow(
    ProvisioningWindowTrigger trigger, int configured_window_seconds);

// How long the descriptor may tell the controller this offer lasts — and
// nothing at all when it does not end. "There is a deadline" and "there is no
// deadline" are two different facts about the offer, so an unbounded window
// cannot produce a number here to be mistaken for one: it returns no value, and
// the descriptor then carries no duration.
//
// This used to hand back 0 for an unbounded window. The controller requires a
// positive duration, so every device that had never been commissioned — every
// device out of the box — advertised a duration the controller read as a broken
// descriptor and refused to connect to. A sentinel is also indistinguishable
// from a field nobody filled in, which is the other way this returns wrong.
std::optional<int> AdvertisedWindowSeconds(const ProvisioningWindowPolicy& policy);

}  // namespace eidolon

#endif  // EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
