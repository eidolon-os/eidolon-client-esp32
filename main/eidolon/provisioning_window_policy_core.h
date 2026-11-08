#ifndef EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
#define EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_

#include <string>

#include "commissioning_orchestrator_core.h"
#include "device_state.h"

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
    // refuses to ask for one while the journal stands (D8, §1 item 10).
    NeverCommissioned,
    // A commissioned device reopened setup on the physical-presence gesture.
    // It holds Owner trust material, so the open offer is the one thing that
    // could hand it to somebody else.
    //
    // The name is a claim about how this trigger is reached, and what makes it
    // true is that the device's automatic doors can no longer produce it: they
    // ask AutomaticSetupOpenIsForbidden first, so a router that rebooted is
    // turned away. What remains are the gestures a person performs at the
    // device.
    OwnerPresenceReopen,
};

// The trust store is the authority on whether this device has an Owner: an
// empty commissioned Owner Domain id is the only evidence of a device that
// belongs to nobody.
ProvisioningWindowTrigger ProvisioningWindowTriggerFor(
    const std::string& commissioned_owner_domain_id);

// Network failure must not open setup on an owned device. Authorization to
// open and the lifetime of an explicitly opened window are independent.
bool AutomaticSetupOpenIsForbidden(ProvisioningWindowTrigger trigger);

// Once open, setup stays available until it completes, a person cancels it at
// the device, or the power goes; no window here ever closes on a timer.
//
// Whether the window is offered to a phone: advertising, or a phone connected
// and not yet applying a network. Its code is on screen exactly then, and the
// setup gesture made at the device closes it exactly then. Once a network is
// being applied the person is turned away until that ends — a rollback in the
// middle of it would undo what the phone is about to report as done — and a
// window still starting or already closing has nothing open to close.
bool SetupWindowIsOffered(CommissioningRuntimeState state);

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
// kDeviceStateConnecting does not mean a sub-second audio-channel open either,
// which is what it meant upstream and why it was first refused here. The
// presenter projects every service phase short of Ready onto it — the first
// control-room join, a Host that restarted or moved, Wi-Fi that went away, a
// conversation that failed — so a device can sit in it indefinitely, on a
// screen that says "Hold BOOT to change network". Nothing in flight there is
// durable: the commissioning actor quiesces the voice runtime and takes the
// radio before it advertises, as it does from idle.
//
// Upgrading and the transient states stay refused: an OTA must not be
// interrupted, and a device that has not finished deciding what it is has
// nothing to hand over yet.
bool HubDeviceStateAllowsSetupOpen(DeviceState state);

// What the screen says when the gesture is turned away: why, and that the same
// press will work once that has passed. Null exactly when setup may open, so a
// refusal cannot be added without something to say.
const char* HubSetupRefusalNotice(DeviceState state);

// A short setup-button click stays useful after boot has advanced into
// activation or recovery. Operational states retain their chat action.
bool HubSetupButtonClickOpensSetup(DeviceState state);

}  // namespace eidolon

#endif  // EIDOLON_PROVISIONING_WINDOW_POLICY_CORE_H_
