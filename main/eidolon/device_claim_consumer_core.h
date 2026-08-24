#ifndef EIDOLON_DEVICE_CLAIM_CONSUMER_CORE_H_
#define EIDOLON_DEVICE_CLAIM_CONSUMER_CORE_H_

#include <cstdint>
#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

enum class EnrollmentJournalPhase : uint8_t {
    ProposalCreated = 1,
    GrantStaged = 2,
};

// Device-local recovery checkpoint. This is not a second wire DTO: every field
// is extracted from, and checked against, the pinned SDK schema/goldens.
struct EnrollmentJournalEntry {
    EnrollmentJournalPhase phase = EnrollmentJournalPhase::ProposalCreated;
    device_foundation::v1::OwnerDomainId owner_domain_id;
    uint64_t owner_domain_generation = 0;
    std::string device_instance_candidate_id;
    std::string enrollment_id;
    uint64_t proposal_revision = 0;
    std::string collection_challenge;
    std::string hardware_evidence_digest;
    device_foundation::v1::ManifestRef manifest_ref;
    std::string handoff_key_id;
    std::string operational_key_id;
    std::string grant_id;
    std::string approval_decision_id;
    device_foundation::v1::DeviceRef staged_device_ref;
};

enum class ActiveClaimLocalState : uint8_t {
    Active = 1,
    Revoked = 2,
};

struct ActiveClaimState {
    device_foundation::v1::DeviceRef device_ref;
    device_foundation::v1::ManifestRef manifest_ref;
    std::string grant_id;
    ActiveClaimLocalState state = ActiveClaimLocalState::Active;

    bool valid() const;
};

enum class ClaimStoreLoadResult {
    NotFound,
    Loaded,
    StorageFailure,
};

class EnrollmentJournalPort {
public:
    virtual ~EnrollmentJournalPort() = default;
    virtual ClaimStoreLoadResult LoadEnrollment(
        EnrollmentJournalEntry& out) = 0;
    virtual bool StoreEnrollment(const EnrollmentJournalEntry& value) = 0;
    virtual bool ClearEnrollment() = 0;
};

class ActiveClaimStorePort {
public:
    virtual ~ActiveClaimStorePort() = default;
    virtual ClaimStoreLoadResult LoadActiveClaim(ActiveClaimState& out) = 0;
    virtual bool StoreActiveClaim(const ActiveClaimState& value) = 0;
};

enum class ClaimGrantUnsealResult {
    Authenticated,
    WireAuthUnavailable,
    AuthenticationRejected,
};

class ClaimGrantCryptoPort {
public:
    virtual ~ClaimGrantCryptoPort() = default;
    virtual std::string HandoffKeyId() const = 0;
    virtual std::string OperationalKeyId() const = 0;
    virtual bool BuildHandoffKeyProof(const std::string& enrollment_id,
                                      uint64_t proposal_revision,
                                      const std::string& collection_challenge,
                                      std::string& proof) = 0;
    // The adapter MUST authenticate envelope.ciphertext with exactly
    // canonical_aad, parse the authenticated plaintext into the generated
    // ClaimGrant binding, and return no plaintext on authentication failure.
    // No production HPKE adapter is implied by this Port.
    virtual ClaimGrantUnsealResult OpenClaimGrant(
        const device_foundation::v1::ClaimGrantWireEnvelope& envelope,
        const std::string& canonical_aad,
        device_foundation::v1::ClaimGrant& plaintext) = 0;
    virtual bool BuildOperationalKeyProof(
        const std::string& enrollment_id,
        const std::string& grant_id,
        const device_foundation::v1::DeviceRef& device_ref,
        std::string& proof) = 0;
    // Must be idempotent: ActiveClaim is committed first, then reboot recovery
    // retries destruction until the Enrollment/handoff material is gone.
    virtual bool DestroyEnrollmentMaterial(
        const std::string& enrollment_id,
        const std::string& handoff_key_id) = 0;
};

enum class DeviceClaimConsumerResult {
    ProposalRecorded,
    CollectionReady,
    GrantStaged,
    AckReady,
    ClaimActivated,
    ClaimRevoked,
    Replayed,
    PendingReview,
    TerminalRevoked,
    NoPendingEnrollment,
    WireAuthUnavailable,
    AuthenticationRejected,
    InvalidContract,
    OwnerDomainMismatch,
    ManifestMismatch,
    StaleGeneration,
    IdempotencyConflict,
    StorageFailure,
};

struct DeviceClaimConsumerOutcome {
    DeviceClaimConsumerResult result =
        DeviceClaimConsumerResult::InvalidContract;
    std::string wire_payload;
};

class DeviceClaimConsumerCore {
public:
    DeviceClaimConsumerCore(EnrollmentJournalPort& enrollment,
                            ActiveClaimStorePort& active_claim,
                            ClaimGrantCryptoPort& crypto)
        : enrollment_(enrollment), active_claim_(active_claim), crypto_(crypto) {}

    DeviceClaimConsumerOutcome RecordProposal(
        const std::string& canonical_create_enrollment,
        const std::string& canonical_create_result,
        uint64_t owner_domain_generation);
    DeviceClaimConsumerOutcome BuildCollectionRequest();
    DeviceClaimConsumerOutcome AcceptCollectedGrant(
        const device_foundation::v1::CollectClaimGrantResult& collect_result);
    DeviceClaimConsumerOutcome BuildGrantAck();
    DeviceClaimConsumerOutcome AcceptGrantAck(
        const std::string& canonical_ack_result);
    DeviceClaimConsumerOutcome ApplyClaimRevoked(
        const device_foundation::v1::DeviceRef& revoked_ref);
    DeviceClaimConsumerOutcome ResumePending();

    static std::string ClaimGrantAad(
        const device_foundation::v1::ClaimGrantAAD& aad);
    static std::string DeviceRefJson(
        const device_foundation::v1::DeviceRef& device_ref);

private:
    DeviceClaimConsumerOutcome AckFor(const EnrollmentJournalEntry& entry);

    EnrollmentJournalPort& enrollment_;
    ActiveClaimStorePort& active_claim_;
    ClaimGrantCryptoPort& crypto_;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_CLAIM_CONSUMER_CORE_H_
