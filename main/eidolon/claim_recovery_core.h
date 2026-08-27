#ifndef EIDOLON_CLAIM_RECOVERY_CORE_H_
#define EIDOLON_CLAIM_RECOVERY_CORE_H_

#include "device_claim_consumer_core.h"
#include "device_foundation_v1_generated.h"

namespace eidolon {

// Why a stored Claim cannot be used against the Authority now answering.
enum class ClaimUsability {
    // The Claim names this device and this Authority generation. Use it.
    Usable,
    // The Owner decided this device is no longer theirs. Their decision, about
    // this device, and it stands until a person is physically at the device.
    OwnerRevoked,
    // Nobody decided anything about this device: the Owner Domain itself moved
    // on — a Host reinstalled or re-keyed — and the Claim names an Authority
    // generation that no longer exists. There is nothing here to respect and
    // nobody to ask, so the Claim is dead weight rather than a decision.
    AuthorityMovedOn,
    // The Claim was issued to a different device principal than the one asking.
    // It can never be used by this device.
    ForeignPrincipal,
};

// What the device may do about it on its own, with nobody at the device.
enum class ClaimRecovery {
    // Keep the Claim and use it.
    Proceed,
    // Drop the dead Claim and propose again. This is asking, not being granted:
    // a fresh Proposal lands in pending-approval, where the Owner decides as
    // they did the first time. Invariant: no transient condition — and a Host
    // being reinstalled is transient — may become a permanent state.
    DropAndRepropose,
    // Stop, and say so. Only physical presence at the device may clear this,
    // because a person deciding to remove their device must not be undone by
    // the device itself.
    RequirePhysicalPresence,
};

// Whether the stored Claim can be used against this descriptor.
//
// The descriptor handed in here has already passed trust verification and the
// Owner-generation rollback check, so a generation that differs from the
// Claim's has genuinely moved forward under the device and cannot have been
// induced by anything on the network without the Owner's signing key.
ClaimUsability ClassifyStoredClaim(
    const ActiveClaimState& claim,
    const std::string& this_device_instance_id,
    const device_foundation::v1::OwnerDomainDescriptor& descriptor);

ClaimRecovery RecoveryForUsability(ClaimUsability usability);

// Whether this device has standing to ask the Authority what the Owner wants
// done with a Claim.
//
// Signing a delivery request with this device's key against a Claim issued to
// some other principal is not a question it is entitled to ask, and the
// Authority refuses it. Every other state — including a Claim the Owner already
// revoked — is this device's own Claim, and being revoked is precisely when the
// Owner is most likely to have left an instruction about it.
bool MayConsultOwnerInstruction(ClaimUsability usability);

// Whether the physical-presence gesture has something to clear, judged from the
// stored Claim alone. At the moment of the gesture the device is talking to no
// Authority, so the Claim's own local state is all there is to go on — which is
// enough, because the states nobody decided (an Authority that moved on) are
// resolved by the device itself and never persist to be found here.
//
// A Claim still marked Active is left alone. Reopening setup is not the same act
// as giving up a Claim, and a long press on a working device must not cost it
// the Claim it is still using.
bool PhysicalPresenceMayClearStoredClaim(const ActiveClaimState& claim);

}  // namespace eidolon

#endif  // EIDOLON_CLAIM_RECOVERY_CORE_H_
