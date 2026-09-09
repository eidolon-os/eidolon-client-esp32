#include "claim_recovery_core.h"

namespace eidolon {

ClaimUsability ClassifyStoredClaim(
    const ActiveClaimState& claim,
    const std::string& this_device_instance_id,
    const device_foundation::v1::OwnerDomainDescriptor& descriptor)
{
    if (claim.device_ref.device_instance_id != this_device_instance_id)
        return ClaimUsability::ForeignPrincipal;
    if (claim.device_ref.owner_domain_id.value != descriptor.owner_domain_id)
        return ClaimUsability::OwnerChanged;
    if (claim.device_ref.owner_domain_generation > descriptor.owner_domain_generation)
        return ClaimUsability::AuthorityRollback;
    if (claim.device_ref.owner_domain_generation < descriptor.owner_domain_generation)
        return ClaimUsability::AuthorityReset;
    return claim.state == ActiveClaimLocalState::Revoked
        ? ClaimUsability::OwnerRevoked : ClaimUsability::Usable;
}

ClaimRecovery RecoveryForUsability(ClaimUsability usability) {
    if (usability == ClaimUsability::Usable) return ClaimRecovery::Proceed;
    return usability == ClaimUsability::OwnerRevoked
        ? ClaimRecovery::RequirePhysicalPresence : ClaimRecovery::RequireAuthorizedRecovery;
}

bool MayConsultOwnerInstruction(ClaimUsability usability) {
    return usability == ClaimUsability::Usable || usability == ClaimUsability::OwnerRevoked;
}

bool PhysicalPresenceMayClearStoredClaim(const ActiveClaimState& claim)
{
    return claim.state == ActiveClaimLocalState::Revoked;
}

}  // namespace eidolon
