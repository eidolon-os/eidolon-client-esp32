#include <cassert>
#include <string>

#include "eidolon/claim_recovery_core.h"

namespace {

using eidolon::ActiveClaimLocalState;
using eidolon::ActiveClaimState;
using eidolon::ClaimRecovery;
using eidolon::ClaimUsability;
using eidolon::ClassifyStoredClaim;
using eidolon::MayConsultOwnerInstruction;
using eidolon::PhysicalPresenceMayClearStoredClaim;
using eidolon::RecoveryForUsability;
using eidolon::device_foundation::v1::OwnerDomainDescriptor;

constexpr char kThisDevice[] = "device-aaaa";
constexpr char kOwner[] = "owner-b0a862b0aab941d64554";

OwnerDomainDescriptor Directory(const char* owner, uint32_t generation)
{
    OwnerDomainDescriptor descriptor;
    descriptor.owner_domain_id = owner;
    descriptor.owner_domain_generation = generation;
    return descriptor;
}

ActiveClaimState Claim(const char* device, const char* owner,
                       uint32_t generation, ActiveClaimLocalState state)
{
    ActiveClaimState claim;
    claim.device_ref.device_instance_id = device;
    claim.device_ref.owner_domain_id.value = owner;
    claim.device_ref.owner_domain_generation = generation;
    claim.state = state;
    return claim;
}

void AWorkingClaimIsLeftAlone()
{
    const auto claim = Claim(kThisDevice, kOwner, 7, ActiveClaimLocalState::Active);
    const auto usability =
        ClassifyStoredClaim(claim, kThisDevice, Directory(kOwner, 7));
    assert(usability == ClaimUsability::Usable);
    assert(RecoveryForUsability(usability) == ClaimRecovery::Proceed);
    // And a long press on a working device must not cost it that Claim.
    assert(!PhysicalPresenceMayClearStoredClaim(claim));
}

// A Host reinstalled or re-keyed moves the Owner Domain generation forward. That
// is not a decision about this device, and it left the device holding a Claim
// nothing could use — terminal, with no way back but erasing NVS, which mints a
// new identity and so returns a different device than the one that left.
void AnAuthorityThatMovedOnIsNotADecisionAboutThisDevice()
{
    const auto claim = Claim(kThisDevice, kOwner, 7, ActiveClaimLocalState::Active);
    const auto usability =
        ClassifyStoredClaim(claim, kThisDevice, Directory(kOwner, 8));
    assert(usability == ClaimUsability::AuthorityMovedOn);
    assert(RecoveryForUsability(usability) == ClaimRecovery::DropAndRepropose);
}

void AClaimNamingAnotherOwnerDomainIsAlsoDead()
{
    const auto usability = ClassifyStoredClaim(
        Claim(kThisDevice, "owner-somebody-else", 7, ActiveClaimLocalState::Active),
        kThisDevice, Directory(kOwner, 7));
    assert(usability == ClaimUsability::AuthorityMovedOn);
    assert(RecoveryForUsability(usability) == ClaimRecovery::DropAndRepropose);
}

// The Owner deciding to remove their device stands. The device does not undo it
// by asking again on its own — but a person at the device may give the Claim up,
// which is the whole difference between "terminal" and "unrecoverable".
void AnOwnerRevocationWaitsForSomebodyAtTheDevice()
{
    const auto claim = Claim(kThisDevice, kOwner, 7, ActiveClaimLocalState::Revoked);
    const auto usability =
        ClassifyStoredClaim(claim, kThisDevice, Directory(kOwner, 7));
    assert(usability == ClaimUsability::OwnerRevoked);
    assert(RecoveryForUsability(usability) ==
           ClaimRecovery::RequirePhysicalPresence);
    assert(PhysicalPresenceMayClearStoredClaim(claim));
}

// A revocation is the Owner's decision even when the Authority has also moved
// on, so it must not be downgraded into something the device may undo alone.
void ARevocationOutranksAnAuthorityThatAlsoMoved()
{
    const auto usability = ClassifyStoredClaim(
        Claim(kThisDevice, kOwner, 7, ActiveClaimLocalState::Revoked),
        kThisDevice, Directory(kOwner, 9));
    assert(usability == ClaimUsability::OwnerRevoked);
    assert(RecoveryForUsability(usability) ==
           ClaimRecovery::RequirePhysicalPresence);
}

// Whatever the Owner decided, they decided it about a different device.
void AClaimIssuedToAnotherPrincipalIsNeverThisDevicesToHonour()
{
    const auto usability = ClassifyStoredClaim(
        Claim("device-bbbb", kOwner, 7, ActiveClaimLocalState::Revoked),
        kThisDevice, Directory(kOwner, 7));
    assert(usability == ClaimUsability::ForeignPrincipal);
    assert(RecoveryForUsability(usability) == ClaimRecovery::DropAndRepropose);
}

// The Owner's own instruction outranks anything the device concludes about its
// Claim. A revoked Claim is exactly when an erase is most likely to be waiting,
// so deciding "revoked, nothing more to do" must not be what stops the device
// collecting it — the Authority re-arms a lapsed delivery for precisely the
// device that was away too long, and a device that stopped listening never
// drops the Owner's data.
void ARevokedClaimStillAsksWhatTheOwnerWantsDone()
{
    assert(MayConsultOwnerInstruction(ClaimUsability::OwnerRevoked));
    assert(MayConsultOwnerInstruction(ClaimUsability::AuthorityMovedOn));
    assert(MayConsultOwnerInstruction(ClaimUsability::Usable));
}

// Signing a delivery request with this device's key against somebody else's
// Claim is not a question it is entitled to ask, and the Authority refuses it.
void AForeignClaimGivesNoStandingToAsk()
{
    assert(!MayConsultOwnerInstruction(ClaimUsability::ForeignPrincipal));
}

// The promise the removal dialog makes: every state a device can be left in is
// correctable, and none of the corrections require erasing its identity.
void EveryDeadClaimHasAWayBack()
{
    const ClaimUsability all[] = {
        ClaimUsability::Usable,        ClaimUsability::OwnerRevoked,
        ClaimUsability::AuthorityMovedOn, ClaimUsability::ForeignPrincipal,
    };
    for (ClaimUsability usability : all) {
        const ClaimRecovery recovery = RecoveryForUsability(usability);
        const bool correctable =
            recovery == ClaimRecovery::Proceed ||
            recovery == ClaimRecovery::DropAndRepropose ||
            recovery == ClaimRecovery::RequirePhysicalPresence;
        assert(correctable);
        // Nothing may be left needing something the person cannot do.
        if (recovery == ClaimRecovery::RequirePhysicalPresence) {
            ActiveClaimState claim;
            claim.state = ActiveClaimLocalState::Revoked;
            assert(PhysicalPresenceMayClearStoredClaim(claim));
        }
    }
}

}  // namespace

int main()
{
    AWorkingClaimIsLeftAlone();
    AnAuthorityThatMovedOnIsNotADecisionAboutThisDevice();
    AClaimNamingAnotherOwnerDomainIsAlsoDead();
    AnOwnerRevocationWaitsForSomebodyAtTheDevice();
    ARevocationOutranksAnAuthorityThatAlsoMoved();
    AClaimIssuedToAnotherPrincipalIsNeverThisDevicesToHonour();
    EveryDeadClaimHasAWayBack();
    ARevokedClaimStillAsksWhatTheOwnerWantsDone();
    AForeignClaimGivesNoStandingToAsk();
    return 0;
}
