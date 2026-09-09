#include "eidolon/claim_recovery_core.h"
#include <cassert>
using namespace eidolon;
int main() {
    ActiveClaimState claim;
    claim.device_ref.owner_domain_id.value = "owner-a";
    claim.device_ref.owner_domain_generation = 7;
    claim.device_ref.device_instance_id = "device-a";
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    descriptor.owner_domain_id = "owner-a";
    descriptor.owner_domain_generation = 7;
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::Usable);
    descriptor.directory_revision = 500; // Host/route/deployment has no Claim effect.
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::Usable);
    descriptor.owner_domain_generation = 8;
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::AuthorityReset);
    descriptor.owner_domain_generation = 6;
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::AuthorityRollback);
    descriptor.owner_domain_id = "owner-b";
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::OwnerChanged);
    claim.state = ActiveClaimLocalState::Revoked;
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::OwnerChanged);
    descriptor.owner_domain_id = "owner-a";
    descriptor.owner_domain_generation = 7;
    assert(ClassifyStoredClaim(claim, "device-a", descriptor) == ClaimUsability::OwnerRevoked);
    assert(ClassifyStoredClaim(claim, "device-b", descriptor) == ClaimUsability::ForeignPrincipal);
    for (const auto reason : {ClaimUsability::OwnerChanged, ClaimUsability::AuthorityReset,
                             ClaimUsability::AuthorityRollback, ClaimUsability::ForeignPrincipal}) {
        assert(RecoveryForUsability(reason) == ClaimRecovery::RequireAuthorizedRecovery);
        assert(!MayConsultOwnerInstruction(reason));
    }
    assert(MayConsultOwnerInstruction(ClaimUsability::OwnerRevoked));
    assert(MayConsultOwnerInstruction(ClaimUsability::Usable));
    assert(PhysicalPresenceMayClearStoredClaim(claim));
    claim.state = ActiveClaimLocalState::Active;
    assert(!PhysicalPresenceMayClearStoredClaim(claim));
}
