#ifndef EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
#define EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_

#include <optional>
#include <string>

#include "device_state.h"
#include "device_foundation_v1_generated.h"

namespace eidolon {

// Why this device is offering to be set up right now. The two cases are not
// variations of one act: they differ in what the device already holds, and
// therefore in what an open setup offer can cost.
enum class ProvisioningWindowTrigger {
    // This device holds no commissioned Owner Domain: factory state, or an
    // Owner erase whose removal a person has since consumed at the device. It
    // has no trust material, no Claim and no network to lose, and nobody has
    // yet told it who it belongs to.
    //
    // "An erase returned it here" is only half the story, and believing that
    // half is what opened a window on a device nobody was standing at. Between
    // the erase and the button press the trust store is empty and the device is
    // still un-claimable, because the RemovalJournal outlives the erase. That
    // window never reaches this decision: WifiBoard::StartWifiConfigMode
    // refuses to ask for one while the journal stands (D8, §1 item 10), so
    // every trigger this enum names is one a person authorized.
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
//
// The canonical duration type is what keeps that fix from being reversible: it
// has no representation for 0, so this is the only place where "bounded" is
// turned into a number, and it looks at no number at all when the window is
// unbounded.
std::optional<device_foundation::v1::SetupWindowRemainingSeconds>
AdvertisedWindowSeconds(const ProvisioningWindowPolicy& policy);

// Which device states the physical-presence gesture may open setup from, in
// HUB_MODE, where the commissioning actor owns the setup window.
//
// kDeviceStateWifiConfiguring does not mean that window is open. It is also
// where Application parks a device whose activation just failed — including a
// device the Owner removed, which is at that moment showing "open setup to
// claim it again". Refusing the gesture there left that device with no way back
// but an NVS erase, and an NVS erase mints a new device identity, so the device
// the Owner removed is gone rather than reclaimed.
//
// Upgrading and the transient states stay refused: an OTA must not be
// interrupted, and a device that has not finished deciding what it is has
// nothing to hand over yet.
bool HubDeviceStateAllowsSetupOpen(DeviceState state);

}  // namespace eidolon

#endif  // EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
