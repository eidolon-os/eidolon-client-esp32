#pragma once
#include "device_claim_consumer_core.h"
#include "device_foundation_v1_generated.h"
namespace eidolon {
enum class ClaimUsability {
    Usable, OwnerRevoked, OwnerChanged, AuthorityReset, AuthorityRollback, ForeignPrincipal,
};
enum class ClaimRecovery { Proceed, RequirePhysicalPresence, RequireAuthorizedRecovery };
// Input descriptor must already have passed signature and directory validation.
ClaimUsability ClassifyStoredClaim(const ActiveClaimState& claim,
    const std::string& this_device_instance_id,
    const device_foundation::v1::OwnerDomainDescriptor& descriptor);
ClaimRecovery RecoveryForUsability(ClaimUsability usability);
bool MayConsultOwnerInstruction(ClaimUsability usability);
bool PhysicalPresenceMayClearStoredClaim(const ActiveClaimState& claim);
}  // namespace eidolon
