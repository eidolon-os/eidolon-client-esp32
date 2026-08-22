#include "owner_trust_commissioner.h"

#include "device_provisioning_protocol.h"
#include "hub_onboarding_protocol.h"

namespace eidolon {

OwnerTrustCommissioningOutcome OwnerTrustCommissioner::Commission(
    const std::string& wire_payload,
    uint32_t setup_generation,
    const std::function<bool()>& commit_guard)
{
    if (setup_generation == 0 || !commit_guard || !commit_guard()) {
        return {OwnerTrustCommissioningCode::Stale, {}};
    }

    TrustHandover handover;
    if (!ParseTrustHandover(wire_payload, handover)) {
        return {OwnerTrustCommissioningCode::Unsupported, {}};
    }

    device_foundation::v1::OwnerDomainDescriptor descriptor;
    std::string canonical;
    if (!ParseOwnerDomainDescriptor(
            handover.owner_domain_descriptor_json, descriptor, canonical) ||
        descriptor.owner_domain_id != handover.owner_domain_id ||
        !verifier_.Verify(
            descriptor,
            canonical,
            handover.owner_root_certificate_pem,
            handover.authority_signing_certificate_pem)) {
        return {OwnerTrustCommissioningCode::Invalid, {}};
    }

    OwnerTrustBundle bundle;
    bundle.owner_domain_id = handover.owner_domain_id;
    bundle.owner_domain_descriptor_json =
        handover.owner_domain_descriptor_json;
    bundle.owner_root_certificate_pem =
        handover.owner_root_certificate_pem;
    bundle.authority_signing_certificate_pem =
        handover.authority_signing_certificate_pem;

    switch (store_.Stage(bundle, setup_generation, commit_guard)) {
    case OwnerTrustStoreResult::Staged:
        return {OwnerTrustCommissioningCode::Staged,
                handover.owner_domain_id};
    case OwnerTrustStoreResult::Stale:
        return {OwnerTrustCommissioningCode::Stale, {}};
    case OwnerTrustStoreResult::Invalid:
        return {OwnerTrustCommissioningCode::Invalid, {}};
    case OwnerTrustStoreResult::Unavailable:
        return {OwnerTrustCommissioningCode::StorageUnavailable, {}};
    }
    return {OwnerTrustCommissioningCode::StorageUnavailable, {}};
}

}  // namespace eidolon
