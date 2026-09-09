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

    OwnerTrustBundle active;
    const auto loaded = store_.ReadActive(active);
    if (loaded == OwnerTrustLoadResult::Unavailable) {
        return {OwnerTrustCommissioningCode::StorageUnavailable, {}};
    }
    bool replace_identity = false;
    if (loaded == OwnerTrustLoadResult::Loaded) {
        device_foundation::v1::OwnerDomainDescriptor previous;
        std::string previous_canonical;
        if (!ParseOwnerDomainDescriptor(active.owner_domain_descriptor_json,
                                        previous, previous_canonical) ||
            previous.owner_domain_id != active.owner_domain_id) {
            return {OwnerTrustCommissioningCode::Invalid, {}};
        }
        replace_identity = active.owner_domain_id != descriptor.owner_domain_id;
        if (!replace_identity) {
            // Setup is not permission to roll back this Owner's accepted floor.
            if (descriptor.owner_domain_generation < previous.owner_domain_generation ||
                (descriptor.owner_domain_generation == previous.owner_domain_generation &&
                 (descriptor.directory_revision < previous.directory_revision ||
                  (descriptor.directory_revision == previous.directory_revision &&
                   (canonical != previous_canonical || descriptor.signature != previous.signature))))) {
                return {OwnerTrustCommissioningCode::Invalid, {}};
            }
            replace_identity = descriptor.owner_domain_generation > previous.owner_domain_generation;
        }
    }
    if (!commit_guard()) return {OwnerTrustCommissioningCode::Stale, {}};
    PreparedCommissioningIdentity identity;
    if (!credentials_.Prepare(descriptor.owner_domain_id,
                              descriptor.owner_domain_generation,
                              setup_generation, replace_identity, identity)) {
        return {OwnerTrustCommissioningCode::StorageUnavailable, {}};
    }
    if (handover.prepare_only) {
        return commit_guard()
            ? OwnerTrustCommissioningOutcome{OwnerTrustCommissioningCode::Prepared,
                                             descriptor.owner_domain_id, identity}
            : OwnerTrustCommissioningOutcome{OwnerTrustCommissioningCode::Stale, {}};
    }
    CommissioningCredential credential;
    const bool has_voucher = !handover.commissioning_voucher.empty();
    if (has_voucher) {
        if (!ParseCommissioningVoucher(handover.commissioning_voucher, credential) ||
            credential.owner_domain_id != descriptor.owner_domain_id ||
            credential.operational_key_id != identity.fingerprint) {
            return {OwnerTrustCommissioningCode::Invalid, {}};
        }
        credential.owner_domain_generation = descriptor.owner_domain_generation;
    }
    if (!credentials_.Stage(has_voucher ? &credential : nullptr,
                            setup_generation, commit_guard)) {
        return {OwnerTrustCommissioningCode::StorageUnavailable, {}};
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
                handover.owner_domain_id, identity};
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
