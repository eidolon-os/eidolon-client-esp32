#include "eidolon/device_claim_consumer_core.h"

#include <cassert>
#include <string>

using namespace eidolon;
using eidolon::device_foundation::v1::ClaimGrant;
using eidolon::device_foundation::v1::ClaimGrantAAD;
using eidolon::device_foundation::v1::CollectClaimGrantResult;
using eidolon::device_foundation::v1::DeviceRef;

namespace {

constexpr const char* kHardwareDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* kManifestDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr const char* kHandoffKeyId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr const char* kOperationalKeyId =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";

std::string CreateEnrollment() {
    return std::string("{\"profile_id\":\"eidolon-trust-p256-hpke-v1\",") +
        "\"device_instance_candidate_id\":\"device_01\"," +
        "\"requested_owner_domain_id\":\"owner-domain_01\"," +
        "\"hardware_identity_evidence\":{" +
        "\"scheme\":\"hub-issued-base-p256\",\"evidence\":\"bWFudWZhY3R1cmVyLWNoYWlu\"," +
        "\"evidence_digest\":\"" + kHardwareDigest + "\"}," +
        "\"commissioning_proof\":{" +
        "\"scheme\":\"hub-issued-commissioning-voucher-v1\"," +
        "\"proof\":\"c3JwNmEtcHJvb2YtYnl0ZXM\",\"nonce\":\"bm9uY2Utbm9uY2Utbm9uY2U\"}," +
        "\"manifest\":{\"manifest_id\":\"manifest_01\",\"revision\":2," +
        "\"digest\":\"" + kManifestDigest + "\",\"document\":{\"endpoints\":[]}}," +
        "\"handoff_key\":{\"scheme\":\"DHKEM-P256-HKDF-SHA256\"," +
        "\"public_key\":\"p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE\"}," +
        "\"operational_key\":{\"scheme\":\"ES256-P256\"," +
        "\"public_key\":\"p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE\"}}";
}

std::string CreateResult() {
    return std::string("{\"enrollment_id\":\"enrollment_01\",") +
        "\"proposal_revision\":1,\"state\":\"pending_review\"," +
        "\"expires_at\":\"2026-08-18T00:15:00Z\"," +
        "\"reviewed_manifest_digest\":\"" + kManifestDigest + "\"," +
        "\"collection_challenge\":\"Y29sbGVjdGlvbi1jaGFsbGVuZ2U\"}";
}

DeviceRef Ref(uint32_t claim_generation = 2) {
    DeviceRef ref;
    ref.device_instance_id = "device_01";
    ref.owner_domain_id.value = "owner-domain_01";
    ref.owner_domain_generation = 3;
    ref.claim_generation = claim_generation;
    ref.trust_epoch = 1;
    return ref;
}

ClaimGrantAAD Aad() {
    ClaimGrantAAD aad;
    aad.enrollment_id = "enrollment_01";
    // This is the immutable Proposal content revision returned to the device
    // by CreateEnrollment. A later Controller Decision advances the Hub's
    // aggregate/source revision, not this proof/AAD precondition.
    aad.proposal_revision = 1;
    aad.device_instance_id = "device_01";
    aad.hardware_evidence_digest = kHardwareDigest;
    aad.manifest_ref = {"manifest_01", 2, kManifestDigest};
    aad.owner_domain_id.value = "owner-domain_01";
    aad.owner_domain_generation = 3;
    aad.claim_generation = 2;
    aad.trust_epoch = 1;
    aad.grant_id = "grant_01";
    return aad;
}

ClaimGrant Grant(const std::string& owner_domain = "owner-domain_01",
                 const std::string& manifest_digest = kManifestDigest,
                 uint32_t claim_generation = 2) {
    ClaimGrant grant;
    grant.grant_id = "grant_01";
    grant.enrollment_id = "enrollment_01";
    grant.device_ref = Ref(claim_generation);
    grant.device_ref.owner_domain_id.value = owner_domain;
    grant.manifest_ref = {"manifest_01", 2, manifest_digest};
    grant.approval_decision_id = "decision_01";
    grant.handoff_key_id = kHandoffKeyId;
    grant.operational_key_id = kOperationalKeyId;
    grant.issued_at = "2026-08-18T00:02:00Z";
    grant.expires_at = "2026-08-18T00:10:00Z";
    return grant;
}

CollectClaimGrantResult CollectResult(
    const std::string& grant = "grant_01",
    const std::string& decision = "decision_01") {
    CollectClaimGrantResult result;
    result.grant_id = grant;
    result.expires_at = "2026-08-18T00:10:00Z";
    result.approval_decision_id = decision;
    result.wire_envelope.recipient_handoff_key_id = kHandoffKeyId;
    result.wire_envelope.encapsulated_key =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    result.wire_envelope.ciphertext = "AAAAAAAAAAAAAAAAAAAAAA";
    result.wire_envelope.aad = Aad();
    result.wire_envelope.aad.grant_id = grant;
    return result;
}

std::string AckResult(const DeviceRef& ref = Ref()) {
    return std::string("{\"device_ref\":") +
        DeviceClaimConsumerCore::DeviceRefJson(ref) +
        ",\"claim_state\":\"active\"}";
}

std::string ReplaceOnce(std::string value, const std::string& from,
                        const std::string& to) {
    const size_t position = value.find(from);
    assert(position != std::string::npos);
    value.replace(position, from.size(), to);
    return value;
}

class Stores final : public EnrollmentJournalPort,
                     public ActiveClaimStorePort {
public:
    ClaimStoreLoadResult LoadEnrollment(EnrollmentJournalEntry& out) override {
        if (fail_load) return ClaimStoreLoadResult::StorageFailure;
        if (!has_enrollment) return ClaimStoreLoadResult::NotFound;
        out = enrollment;
        return ClaimStoreLoadResult::Loaded;
    }
    bool StoreEnrollment(const EnrollmentJournalEntry& value) override {
        if (fail_store_enrollment) return false;
        enrollment = value;
        has_enrollment = true;
        return true;
    }
    bool ClearEnrollment() override {
        ++clear_calls;
        if (fail_clear) return false;
        has_enrollment = false;
        enrollment = {};
        return true;
    }
    ClaimStoreLoadResult LoadActiveClaim(ActiveClaimState& out) override {
        if (fail_load) return ClaimStoreLoadResult::StorageFailure;
        if (!has_active) return ClaimStoreLoadResult::NotFound;
        out = active;
        return ClaimStoreLoadResult::Loaded;
    }
    bool StoreActiveClaim(const ActiveClaimState& value) override {
        if (fail_store_active) return false;
        active = value;
        has_active = true;
        return true;
    }

    bool has_enrollment = false;
    bool has_active = false;
    bool fail_load = false;
    bool fail_store_enrollment = false;
    bool fail_store_active = false;
    bool fail_clear = false;
    int clear_calls = 0;
    EnrollmentJournalEntry enrollment;
    ActiveClaimState active;
};

class Crypto final : public ClaimGrantCryptoPort {
public:
    std::string HandoffKeyId() const override { return kHandoffKeyId; }
    std::string OperationalKeyId() const override { return kOperationalKeyId; }
    bool BuildHandoffKeyProof(const std::string&, uint64_t,
                              const std::string&, std::string& proof) override {
        if (!available) return false;
        proof = "aGFuZG9mZi1rZXktcHJvb2Y";
        return true;
    }
    ClaimGrantUnsealResult OpenClaimGrant(
        const device_foundation::v1::ClaimGrantWireEnvelope& envelope,
        const std::string& canonical_aad, ClaimGrant& out) override {
        ++open_calls;
        if (!available) return ClaimGrantUnsealResult::WireAuthUnavailable;
        if (reject) return ClaimGrantUnsealResult::AuthenticationRejected;
        if (envelope.recipient_handoff_key_id != kHandoffKeyId ||
            envelope.encapsulated_key !=
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" ||
            envelope.ciphertext != "AAAAAAAAAAAAAAAAAAAAAA" ||
            canonical_aad != expected_aad) {
            return ClaimGrantUnsealResult::AuthenticationRejected;
        }
        out = plaintext;
        return ClaimGrantUnsealResult::Authenticated;
    }
    bool BuildOperationalKeyProof(const std::string&, const std::string&,
                                  const DeviceRef&, std::string& proof) override {
        if (!available) return false;
        proof = "b3BlcmF0aW9uYWwta2V5LXByb29m";
        return true;
    }
    bool DestroyEnrollmentMaterial(const std::string& enrollment_id,
                                   const std::string& handoff_key_id) override {
        ++destroy_calls;
        assert(enrollment_id == "enrollment_01");
        assert(handoff_key_id == kHandoffKeyId);
        return !fail_destroy;
    }

    void SetValidGrant(const EnrollmentJournalEntry&,
                       const ClaimGrant& grant = Grant()) {
        plaintext = grant;
        expected_aad = DeviceClaimConsumerCore::ClaimGrantAad(Aad());
    }

    bool available = true;
    bool reject = false;
    bool fail_destroy = false;
    int destroy_calls = 0;
    int open_calls = 0;
    ClaimGrant plaintext;
    std::string expected_aad;
};

void Record(Stores& stores, Crypto& crypto, DeviceClaimConsumerCore& core) {
    assert(core.RecordProposal(CreateEnrollment(), CreateResult(), 3).result ==
           DeviceClaimConsumerResult::ProposalRecorded);
    crypto.SetValidGrant(stores.enrollment);
}

void GoldenAadAndFiveFieldDeviceRefAreExact() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    const std::string expected =
        std::string("{\"claim_generation\":2,") +
        "\"contract\":\"eidolon.device-foundation.claim-grant-aad\"," +
        "\"device_instance_id\":\"device_01\"," +
        "\"enrollment_id\":\"enrollment_01\",\"grant_id\":\"grant_01\"," +
        "\"hardware_evidence_digest\":\"" + kHardwareDigest + "\"," +
        "\"manifest_ref\":{\"digest\":\"" + kManifestDigest +
        "\",\"manifest_id\":\"manifest_01\",\"revision\":2}," +
        "\"owner_domain_generation\":3,\"owner_domain_id\":\"owner-domain_01\"," +
        "\"profile_id\":\"eidolon-trust-p256-hpke-v1\"," +
        "\"proposal_revision\":1,\"trust_epoch\":1}";
    assert(crypto.expected_aad == expected);
    const std::string ref = DeviceClaimConsumerCore::DeviceRefJson(Ref());
    assert(ref ==
           "{\"claim_generation\":2,\"device_instance_id\":\"device_01\","
           "\"owner_domain_generation\":3,\"owner_domain_id\":\"owner-domain_01\","
           "\"trust_epoch\":1}");
}

void CanonicalProposalParsingFailsClosed() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    assert(core.RecordProposal(
               ReplaceOnce(CreateEnrollment(), "owner-domain_01", "owner_01"),
               CreateResult(), 3)
               .result == DeviceClaimConsumerResult::InvalidContract);
    assert(core.RecordProposal(
               ReplaceOnce(CreateEnrollment(), "\"scheme\":\"ES256-P256\"",
                           "\"scheme\":\"not-es256\""),
               CreateResult(), 3)
               .result == DeviceClaimConsumerResult::InvalidContract);
    assert(core.RecordProposal(
               CreateEnrollment(),
               ReplaceOnce(CreateResult(),
                           "Y29sbGVjdGlvbi1jaGFsbGVuZ2U", "bad+challenge"),
               3)
               .result == DeviceClaimConsumerResult::InvalidContract);
    assert(!stores.has_enrollment);
}

void ProposalCollectionGrantAckAndActivationResumeForwardOnly() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    const auto collection = core.BuildCollectionRequest();
    assert(collection.result == DeviceClaimConsumerResult::CollectionReady);
    assert(collection.wire_payload.find("collection_challenge") !=
           std::string::npos);
    assert(collection.wire_payload.find("\"proposal_revision\":1") !=
           std::string::npos);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    assert(stores.enrollment.phase == EnrollmentJournalPhase::GrantStaged);

    DeviceClaimConsumerCore rebooted(stores, stores, crypto);
    const auto resumed = rebooted.ResumePending();
    assert(resumed.result == DeviceClaimConsumerResult::AckReady);
    assert(resumed.wire_payload.find("stored_claim_generation\":2") !=
           std::string::npos);
    assert(rebooted.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::ClaimActivated);
    assert(stores.has_active && !stores.has_enrollment);
    assert(stores.active.state == ActiveClaimLocalState::Active);
    assert(crypto.destroy_calls == 1);
    assert(rebooted.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::Replayed);
    assert(rebooted.AcceptGrantAck(AckResult(Ref(1))).result ==
           DeviceClaimConsumerResult::StaleGeneration);
}

void ActiveClaimCommitWinsPowerLossBeforeEnrollmentCleanup() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    stores.fail_clear = true;
    assert(core.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::StorageFailure);
    assert(stores.has_active && stores.has_enrollment);
    stores.fail_clear = false;
    DeviceClaimConsumerCore rebooted(stores, stores, crypto);
    assert(rebooted.ResumePending().result ==
           DeviceClaimConsumerResult::Replayed);
    assert(!stores.has_enrollment);
    assert(crypto.destroy_calls == 2);
}

void ActiveClaimCommitWinsPowerLossBeforeMaterialDestruction() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    crypto.fail_destroy = true;
    assert(core.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::StorageFailure);
    assert(stores.has_active && stores.has_enrollment);
    crypto.fail_destroy = false;
    DeviceClaimConsumerCore rebooted(stores, stores, crypto);
    assert(rebooted.ResumePending().result ==
           DeviceClaimConsumerResult::Replayed);
    assert(!stores.has_enrollment);
    assert(crypto.destroy_calls == 2);
}

void ActiveClaimCannotClearAnUnrelatedEnrollmentGeneration() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    stores.active = {Ref(3), stores.enrollment.manifest_ref, "grant_new",
                     ActiveClaimLocalState::Active};
    stores.has_active = true;
    assert(core.ResumePending().result ==
           DeviceClaimConsumerResult::StorageFailure);
    assert(stores.has_enrollment);
    assert(crypto.destroy_calls == 0);
}

void WireAuthAbsenceAndAadMutationNeverStageOrActivate() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    crypto.available = false;
    assert(core.BuildCollectionRequest().result ==
           DeviceClaimConsumerResult::WireAuthUnavailable);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::WireAuthUnavailable);
    assert(stores.enrollment.phase == EnrollmentJournalPhase::ProposalCreated);
    crypto.available = true;
    crypto.expected_aad += " ";
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::AuthenticationRejected);
    assert(!stores.has_active);
}

void EveryEnvelopeAndAadMutationFailsClosedBeforeStaging() {
    const auto run = [](auto mutate, DeviceClaimConsumerResult expected,
                        int expected_open_calls) {
        Stores stores;
        Crypto crypto;
        DeviceClaimConsumerCore core(stores, stores, crypto);
        Record(stores, crypto, core);
        auto collected = CollectResult();
        mutate(collected);
        assert(core.AcceptCollectedGrant(collected).result == expected);
        assert(crypto.open_calls == expected_open_calls);
        assert(stores.enrollment.phase ==
               EnrollmentJournalPhase::ProposalCreated);
        assert(!stores.has_active);
    };

    run([](auto& value) { value.wire_envelope.contract = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) { value.wire_envelope.profile_id = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) { value.wire_envelope.kem = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) { value.wire_envelope.kdf = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) { value.wire_envelope.aead = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) {
            value.wire_envelope.recipient_handoff_key_id =
                "sha256:5555555555555555555555555555555555555555555555555555555555555555";
        },
        DeviceClaimConsumerResult::AuthenticationRejected, 0);
    run([](auto& value) { value.wire_envelope.encapsulated_key[0] = 'B'; },
        DeviceClaimConsumerResult::AuthenticationRejected, 1);
    run([](auto& value) { value.wire_envelope.ciphertext[0] = 'B'; },
        DeviceClaimConsumerResult::AuthenticationRejected, 1);
    run([](auto& value) {
            value.wire_envelope.aad.owner_domain_id.value = "owner-domain_02";
        },
        DeviceClaimConsumerResult::OwnerDomainMismatch, 0);
    run([](auto& value) { value.wire_envelope.aad.contract = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) { value.wire_envelope.aad.profile_id = "other"; },
        DeviceClaimConsumerResult::InvalidContract, 0);
    run([](auto& value) {
            value.wire_envelope.aad.enrollment_id = "enrollment_02";
        },
        DeviceClaimConsumerResult::AuthenticationRejected, 0);
    run([](auto& value) {
            value.wire_envelope.aad.owner_domain_generation = 4;
        },
        DeviceClaimConsumerResult::OwnerDomainMismatch, 0);
    run([](auto& value) {
            value.wire_envelope.aad.device_instance_id = "device_02";
        },
        DeviceClaimConsumerResult::OwnerDomainMismatch, 0);
    run([](auto& value) { value.wire_envelope.aad.proposal_revision = 3; },
        DeviceClaimConsumerResult::StaleGeneration, 0);
    run([](auto& value) {
            value.wire_envelope.aad.hardware_evidence_digest =
                "sha256:9999999999999999999999999999999999999999999999999999999999999999";
        },
        DeviceClaimConsumerResult::AuthenticationRejected, 0);
    run([](auto& value) {
            value.wire_envelope.aad.manifest_ref.digest =
                "sha256:9999999999999999999999999999999999999999999999999999999999999999";
        },
        DeviceClaimConsumerResult::ManifestMismatch, 0);
    run([](auto& value) {
            value.wire_envelope.aad.manifest_ref.manifest_id = "manifest_02";
        },
        DeviceClaimConsumerResult::ManifestMismatch, 0);
    run([](auto& value) { value.wire_envelope.aad.manifest_ref.revision = 3; },
        DeviceClaimConsumerResult::ManifestMismatch, 0);
    run([](auto& value) { value.wire_envelope.aad.claim_generation = 3; },
        DeviceClaimConsumerResult::AuthenticationRejected, 1);
    run([](auto& value) { value.wire_envelope.aad.trust_epoch = 2; },
        DeviceClaimConsumerResult::AuthenticationRejected, 1);
    run([](auto& value) {
            value.grant_id = "grant_02";
            value.wire_envelope.aad.grant_id = "grant_02";
        },
        DeviceClaimConsumerResult::AuthenticationRejected, 1);
}

void PlaintextGrantMustMatchEnvelopeAadAndLocalPreconditions() {
    const auto run = [](auto mutate, DeviceClaimConsumerResult expected) {
        Stores stores;
        Crypto crypto;
        DeviceClaimConsumerCore core(stores, stores, crypto);
        Record(stores, crypto, core);
        mutate(crypto.plaintext);
        assert(core.AcceptCollectedGrant(CollectResult()).result == expected);
        assert(crypto.open_calls == 1);
        assert(stores.enrollment.phase ==
               EnrollmentJournalPhase::ProposalCreated);
        assert(!stores.has_active);
    };

    run([](auto& grant) { grant.grant_id = "grant_02"; },
        DeviceClaimConsumerResult::AuthenticationRejected);
    run([](auto& grant) { grant.enrollment_id = "enrollment_02"; },
        DeviceClaimConsumerResult::AuthenticationRejected);
    run([](auto& grant) {
            grant.device_ref.owner_domain_id.value = "owner-domain_02";
        },
        DeviceClaimConsumerResult::OwnerDomainMismatch);
    run([](auto& grant) { grant.device_ref.claim_generation = 3; },
        DeviceClaimConsumerResult::StaleGeneration);
    run([](auto& grant) {
            grant.manifest_ref.digest =
                "sha256:9999999999999999999999999999999999999999999999999999999999999999";
        },
        DeviceClaimConsumerResult::ManifestMismatch);
    run([](auto& grant) { grant.approval_decision_id = "decision_02"; },
        DeviceClaimConsumerResult::AuthenticationRejected);
    run([](auto& grant) {
            grant.handoff_key_id =
                "sha256:5555555555555555555555555555555555555555555555555555555555555555";
        },
        DeviceClaimConsumerResult::AuthenticationRejected);
    run([](auto& grant) {
            grant.operational_key_id =
                "sha256:5555555555555555555555555555555555555555555555555555555555555555";
        },
        DeviceClaimConsumerResult::AuthenticationRejected);
    run([](auto& grant) { grant.issued_at = "not-a-date"; },
        DeviceClaimConsumerResult::InvalidContract);
    run([](auto& grant) { grant.expires_at = "2026-08-18T00:11:00Z"; },
        DeviceClaimConsumerResult::AuthenticationRejected);
}

void OwnerDomainBusinessOwnerManifestAndGenerationCannotBeInterchanged() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    crypto.plaintext = Grant("owner_01");  // business Owner, not Owner Domain
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::InvalidContract);

    crypto.plaintext = Grant("owner-domain_02");
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::OwnerDomainMismatch);

    crypto.plaintext = Grant("owner-domain_01", "sha256:" + std::string(64, '9'));
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::ManifestMismatch);

    crypto.SetValidGrant(stores.enrollment);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    assert(core.AcceptGrantAck(AckResult(Ref(3))).result ==
           DeviceClaimConsumerResult::StaleGeneration);
    assert(!stores.has_active);
}

void DuplicateGrantAndRevokedTerminalFenceOldGeneration() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::Replayed);
    assert(core.AcceptCollectedGrant(CollectResult("grant_other")).result ==
           DeviceClaimConsumerResult::IdempotencyConflict);
    assert(core.AcceptCollectedGrant(
               CollectResult("grant_01", "decision_other")).result ==
           DeviceClaimConsumerResult::IdempotencyConflict);
    assert(core.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::ClaimActivated);
    assert(core.ApplyClaimRevoked(Ref()).result ==
           DeviceClaimConsumerResult::ClaimRevoked);
    assert(core.ApplyClaimRevoked(Ref()).result ==
           DeviceClaimConsumerResult::Replayed);
    assert(core.ApplyClaimRevoked(Ref(1)).result ==
           DeviceClaimConsumerResult::StaleGeneration);
    assert(core.RecordProposal(CreateEnrollment(), CreateResult(), 3).result ==
           DeviceClaimConsumerResult::TerminalRevoked);
    assert(core.ResumePending().result ==
           DeviceClaimConsumerResult::TerminalRevoked);
}

void PreActiveRevokeCannotFabricateAClaim() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.ApplyClaimRevoked(Ref()).result ==
           DeviceClaimConsumerResult::NoPendingEnrollment);
    assert(!stores.has_active && stores.has_enrollment);
}

void AFinishedProposalIsAbandonedSoAnotherCanBeMade() {
    // A Proposal expired before anyone approved it. The Authority answers that
    // it is gone, forever, so keeping the checkpoint means asking about it
    // forever and never proposing again — which is what a real BOX-3 did for a
    // day, retrying one dead Enrollment every 120 seconds.
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(stores.has_enrollment);

    const auto abandoned = core.AbandonPendingProposal();
    assert(abandoned.result == DeviceClaimConsumerResult::ProposalAbandoned);
    assert(!stores.has_enrollment);
    // The handoff material went with it: the next Proposal is a new Proposal,
    // not a replay of the dead one under a reused idempotency key.
    assert(crypto.destroy_calls == 1);
    assert(core.ResumePending().result ==
           DeviceClaimConsumerResult::NoPendingEnrollment);

    // Idempotent, so a reboot between clearing and the next attempt is not a
    // second failure mode.
    assert(core.AbandonPendingProposal().result ==
           DeviceClaimConsumerResult::NoPendingEnrollment);
}

void AbandoningIsRefusedWhileAClaimIsActive() {
    // An answer about a Proposal is never authority over a Claim. Removal and
    // revocation are their own paths, with their own proofs.
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::GrantStaged);
    assert(core.BuildGrantAck().result == DeviceClaimConsumerResult::AckReady);
    assert(core.AcceptGrantAck(AckResult()).result ==
           DeviceClaimConsumerResult::ClaimActivated);
    assert(stores.has_active);

    const int destroyed = crypto.destroy_calls;
    assert(core.AbandonPendingProposal().result ==
           DeviceClaimConsumerResult::InvalidContract);
    assert(stores.has_active);
    assert(crypto.destroy_calls == destroyed);
}

void AbandoningSurvivesMaterialThatAlreadyRotated() {
    // The material named by the checkpoint is already gone — destroyed, or
    // rotated by a new instance. Refusing to clear the entry would leave the
    // device asking about a Proposal it can no longer prove anything for.
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    stores.enrollment.handoff_key_id =
        "sha256:9999999999999999999999999999999999999999999999999999999999999999";

    assert(core.AbandonPendingProposal().result ==
           DeviceClaimConsumerResult::ProposalAbandoned);
    assert(!stores.has_enrollment);
    assert(crypto.destroy_calls == 0);
}

void StorageThatCannotForgetIsNotReportedAsProgress() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    stores.fail_clear = true;
    assert(core.AbandonPendingProposal().result ==
           DeviceClaimConsumerResult::StorageFailure);
    assert(stores.has_enrollment);

    stores.fail_clear = false;
    crypto.fail_destroy = true;
    assert(core.AbandonPendingProposal().result ==
           DeviceClaimConsumerResult::StorageFailure);
    assert(stores.has_enrollment);
}

}  // namespace

int main() {
    GoldenAadAndFiveFieldDeviceRefAreExact();
    CanonicalProposalParsingFailsClosed();
    ProposalCollectionGrantAckAndActivationResumeForwardOnly();
    ActiveClaimCommitWinsPowerLossBeforeEnrollmentCleanup();
    ActiveClaimCommitWinsPowerLossBeforeMaterialDestruction();
    ActiveClaimCannotClearAnUnrelatedEnrollmentGeneration();
    WireAuthAbsenceAndAadMutationNeverStageOrActivate();
    EveryEnvelopeAndAadMutationFailsClosedBeforeStaging();
    PlaintextGrantMustMatchEnvelopeAadAndLocalPreconditions();
    OwnerDomainBusinessOwnerManifestAndGenerationCannotBeInterchanged();
    DuplicateGrantAndRevokedTerminalFenceOldGeneration();
    PreActiveRevokeCannotFabricateAClaim();
    AFinishedProposalIsAbandonedSoAnotherCanBeMade();
    AbandoningIsRefusedWhileAClaimIsActive();
    AbandoningSurvivesMaterialThatAlreadyRotated();
    StorageThatCannotForgetIsNotReportedAsProgress();
    return 0;
}
