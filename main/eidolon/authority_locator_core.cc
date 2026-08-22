#include "authority_locator_core.h"

#include <algorithm>
#include <tuple>

namespace eidolon {

AuthorityLocatorCore::AuthorityLocatorCore(
    AuthorityDescriptorVerifierPort& verifier,
    AuthorityDescriptorStorePort& store)
    : verifier_(verifier), store_(store)
{
}

void AuthorityLocatorCore::Commission(const std::string& owner_domain_id)
{
    owner_domain_id_ = owner_domain_id;
    accepted_ = device_foundation::v1::OwnerDomainDescriptor{};
    accepted_canonical_.clear();
    has_accepted_ = false;
}

AuthorityLocatorResult AuthorityLocatorCore::Restore(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& canonical_signing_bytes)
{
    if (owner_domain_id_.empty()) {
        return AuthorityLocatorResult::NotCommissioned;
    }
    if (descriptor.owner_domain_id != owner_domain_id_) {
        return AuthorityLocatorResult::WrongOwnerDomain;
    }
    if (!verifier_.Verify(descriptor, canonical_signing_bytes)) {
        return AuthorityLocatorResult::DescriptorRejected;
    }
    accepted_ = descriptor;
    accepted_canonical_ = canonical_signing_bytes;
    has_accepted_ = true;
    return AuthorityLocatorResult::Accepted;
}

AuthorityLocatorResult AuthorityLocatorCore::Accept(
    const device_foundation::v1::OwnerDomainDescriptor& candidate,
    const std::string& canonical_signing_bytes,
    const std::string& descriptor_json)
{
    if (owner_domain_id_.empty()) {
        return AuthorityLocatorResult::NotCommissioned;
    }
    if (candidate.owner_domain_id != owner_domain_id_) {
        return AuthorityLocatorResult::WrongOwnerDomain;
    }
    if (!verifier_.Verify(candidate, canonical_signing_bytes)) {
        return AuthorityLocatorResult::DescriptorRejected;
    }
    if (has_accepted_) {
        if (candidate.directory_revision < accepted_.directory_revision) {
            return AuthorityLocatorResult::RevisionRollback;
        }
        if (candidate.directory_revision == accepted_.directory_revision) {
            if (canonical_signing_bytes != accepted_canonical_ ||
                candidate.signature != accepted_.signature) {
                return AuthorityLocatorResult::RevisionConflict;
            }
            return AuthorityLocatorResult::Unchanged;
        }
    }
    if (descriptor_json.empty() ||
        !store_.SaveAcceptedDescriptor(descriptor_json)) {
        return AuthorityLocatorResult::PersistenceFailed;
    }
    accepted_ = candidate;
    accepted_canonical_ = canonical_signing_bytes;
    has_accepted_ = true;
    return AuthorityLocatorResult::Accepted;
}

AuthorityLocatorResult AuthorityLocatorCore::Resolve(
    const std::string& owner_domain_id,
    device_foundation::v1::LogicalAuthority authority,
    std::vector<const device_foundation::v1::AuthorityEndpoint*>& out) const
{
    out.clear();
    if (owner_domain_id_.empty()) {
        return AuthorityLocatorResult::NotCommissioned;
    }
    if (owner_domain_id != owner_domain_id_) {
        return AuthorityLocatorResult::WrongOwnerDomain;
    }
    if (!has_accepted_ || !verifier_.IsUsableNow(accepted_)) {
        return AuthorityLocatorResult::AuthorityDiscoveryRequired;
    }
    for (const auto& endpoint : accepted_.endpoints) {
        if (endpoint.authority == authority) {
            out.push_back(&endpoint);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto* left, const auto* right) {
        return std::tie(left->priority, left->logical_audience, left->uri,
                        left->transport_profile) <
               std::tie(right->priority, right->logical_audience, right->uri,
                        right->transport_profile);
    });
    return out.empty() ? AuthorityLocatorResult::AuthorityUnavailable
                       : AuthorityLocatorResult::Accepted;
}

const device_foundation::v1::OwnerDomainDescriptor*
AuthorityLocatorCore::accepted() const
{
    return has_accepted_ ? &accepted_ : nullptr;
}

}  // namespace eidolon
