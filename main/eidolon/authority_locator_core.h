#ifndef EIDOLON_AUTHORITY_LOCATOR_CORE_H_
#define EIDOLON_AUTHORITY_LOCATOR_CORE_H_

#include <string>
#include <vector>

#include "device_foundation_v1_generated.h"

namespace eidolon {

// These ports keep trust verification and durable storage outside the Core.
// The Core owns acceptance and resolution policy; adapters may not reproduce it.
class AuthorityDescriptorVerifierPort {
public:
    virtual ~AuthorityDescriptorVerifierPort() = default;
    virtual bool Verify(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& canonical_signing_bytes) const = 0;
    virtual bool IsUsableNow(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor) const = 0;
};

class AuthorityDescriptorStorePort {
public:
    virtual ~AuthorityDescriptorStorePort() = default;
    virtual bool SaveAcceptedDescriptor(const std::string& descriptor_json) = 0;
};

enum class AuthorityLocatorResult {
    Accepted,
    Unchanged,
    NotCommissioned,
    WrongOwnerDomain,
    DescriptorRejected,
    OwnerGenerationRollback,
    OwnerGenerationAdvanced,
    RevisionRollback,
    RevisionConflict,
    PersistenceFailed,
    AuthorityDiscoveryRequired,
    AuthorityUnavailable,
};

// Pure, Host-independent device-side Authority directory state machine.
// It performs no DNS, HTTP, mDNS, NVS, certificate, clock, or ESP-IDF work.
class AuthorityLocatorCore {
public:
    AuthorityLocatorCore(AuthorityDescriptorVerifierPort& verifier,
                         AuthorityDescriptorStorePort& store);

    void Commission(const std::string& owner_domain_id);

    // Restore is used only for the descriptor already committed with the Owner
    // trust bundle. It validates but never rewrites durable state.
    AuthorityLocatorResult Restore(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& canonical_signing_bytes);

    // Accept is the sole update path. A new revision becomes visible in memory
    // only after the adapter has durably committed the exact signed document.
    AuthorityLocatorResult Accept(
        const device_foundation::v1::OwnerDomainDescriptor& candidate,
        const std::string& canonical_signing_bytes,
        const std::string& descriptor_json);

    AuthorityLocatorResult Resolve(
        const std::string& owner_domain_id,
        device_foundation::v1::LogicalAuthority authority,
        std::vector<const device_foundation::v1::AuthorityEndpoint*>& out) const;

    const device_foundation::v1::OwnerDomainDescriptor* accepted() const;

private:
    AuthorityDescriptorVerifierPort& verifier_;
    AuthorityDescriptorStorePort& store_;
    std::string owner_domain_id_;
    device_foundation::v1::OwnerDomainDescriptor accepted_;
    std::string accepted_canonical_;
    bool has_accepted_ = false;
};

}  // namespace eidolon

#endif  // EIDOLON_AUTHORITY_LOCATOR_CORE_H_
