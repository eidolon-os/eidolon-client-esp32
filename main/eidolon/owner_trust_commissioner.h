#ifndef EIDOLON_OWNER_TRUST_COMMISSIONER_H_
#define EIDOLON_OWNER_TRUST_COMMISSIONER_H_

#include <functional>
#include <cstdint>
#include <string>

#include "commissioning_credential.h"
#include "device_foundation_v1_generated.h"

namespace eidolon {

struct OwnerTrustBundle {
    std::string owner_domain_id;
    std::string owner_domain_descriptor_json;
    std::string owner_root_certificate_pem;
    std::string authority_signing_certificate_pem;
};

// Core-facing cryptographic boundary. Implementations may use a secure element
// or a software crypto library; the commissioning policy does not know which.
class OwnerTrustVerifierPort {
public:
    virtual ~OwnerTrustVerifierPort() = default;

    virtual bool Verify(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& canonical_signing_bytes,
        const std::string& owner_root_certificate_pem,
        const std::string& authority_signing_certificate_pem) = 0;
};

enum class OwnerTrustStoreResult {
    Staged,
    Invalid,
    Stale,
    Unavailable,
};

// Where the standing this commissioning grants is kept.
//
// Deliberately not the Owner trust store: the base identity has to live with
// the operational key it is bound to, so that erasing one erases the other. A
// device holding a base identity it can no longer prove is a Body claiming a
// lineage it cannot demonstrate, and that state must not be reachable.
class CommissioningCredentialStorePort {
public:
    virtual ~CommissioningCredentialStorePort() = default;

    virtual bool Save(const CommissioningCredential& credential) = 0;
};

// The store must make the verified bundle durable but not operationally
// visible. Trust becomes active only when the commissioning transaction later
// commits both trust and the validated network candidate.
class OwnerTrustStorePort {
public:
    virtual ~OwnerTrustStorePort() = default;

    virtual OwnerTrustStoreResult Stage(
        const OwnerTrustBundle& bundle,
        uint32_t setup_generation,
        const std::function<bool()>& commit_guard) = 0;
};

enum class OwnerTrustCommissioningCode {
    Staged,
    Unsupported,
    Invalid,
    Stale,
    StorageUnavailable,
};

struct OwnerTrustCommissioningOutcome {
    OwnerTrustCommissioningCode code = OwnerTrustCommissioningCode::Invalid;
    std::string owner_domain_id;
};

// Host-, transport-, radio- and storage-independent application service.
// Exactly one worker invokes an instance at runtime; native tests can exercise
// the same policy with deterministic verifier/store ports.
class OwnerTrustCommissioner {
public:
    OwnerTrustCommissioner(OwnerTrustVerifierPort& verifier,
                           OwnerTrustStorePort& store,
                           CommissioningCredentialStorePort& credentials)
        : verifier_(verifier), store_(store), credentials_(credentials) {}

    OwnerTrustCommissioningOutcome Commission(
        const std::string& wire_payload,
        uint32_t setup_generation,
        const std::function<bool()>& commit_guard);

private:
    OwnerTrustVerifierPort& verifier_;
    OwnerTrustStorePort& store_;
    CommissioningCredentialStorePort& credentials_;
};

}  // namespace eidolon

#endif  // EIDOLON_OWNER_TRUST_COMMISSIONER_H_
