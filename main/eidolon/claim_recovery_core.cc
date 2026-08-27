#include "claim_recovery_core.h"

namespace eidolon {

ClaimUsability ClassifyStoredClaim(
    const ActiveClaimState& claim,
    const std::string& this_device_instance_id,
    const device_foundation::v1::OwnerDomainDescriptor& descriptor)
{
    // A principal mismatch outranks everything else: whatever the Owner decided,
    // they decided it about a different device.
    if (claim.device_ref.device_instance_id != this_device_instance_id) {
        return ClaimUsability::ForeignPrincipal;
    }
    if (claim.state == ActiveClaimLocalState::Revoked) {
        return ClaimUsability::OwnerRevoked;
    }
    if (claim.device_ref.owner_domain_id.value != descriptor.owner_domain_id ||
        claim.device_ref.owner_domain_generation !=
            descriptor.owner_domain_generation) {
        return ClaimUsability::AuthorityMovedOn;
    }
    return ClaimUsability::Usable;
}

ClaimRecovery RecoveryForUsability(ClaimUsability usability)
{
    switch (usability) {
    case ClaimUsability::Usable:
        return ClaimRecovery::Proceed;
    case ClaimUsability::AuthorityMovedOn:
    case ClaimUsability::ForeignPrincipal:
        return ClaimRecovery::DropAndRepropose;
    case ClaimUsability::OwnerRevoked:
        return ClaimRecovery::RequirePhysicalPresence;
    }
    return ClaimRecovery::RequirePhysicalPresence;
}

bool MayConsultOwnerInstruction(ClaimUsability usability)
{
    return usability != ClaimUsability::ForeignPrincipal;
}

bool PhysicalPresenceMayClearStoredClaim(const ActiveClaimState& claim)
{
    return claim.state == ActiveClaimLocalState::Revoked;
}

}  // namespace eidolon
