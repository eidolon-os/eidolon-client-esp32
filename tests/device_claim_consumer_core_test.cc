#include "eidolon/device_claim_consumer_core.h"

#include <cassert>
#include <string>

using namespace eidolon;
using eidolon::device_foundation::v1::DeviceRef;

namespace {

constexpr const char* kHardwareDigest =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr const char* kManifestDigest =
    "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr const char* kHandoffKeyId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr const char* kOperationalKeyId =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";

std::string CreateEnrollment() {
    return std::string("{\"profile_id\":\"eidolon-trust-p256-hpke-v1\",") +
        "\"device_instance_candidate_id\":\"device_01\"," +
        "\"requested_owner_domain_id\":\"owner-domain_01\"," +
        "\"hardware_identity_evidence\":{" +
        "\"scheme\":\"manufacturer-p256\",\"evidence\":\"bWFudWZhY3R1cmVyLWNoYWlu\"," +
        "\"evidence_digest\":\"" + kHardwareDigest + "\"}," +
        "\"commissioning_proof\":{" +
        "\"scheme\":\"protocomm-security2-srp6a-aes256gcm\"," +
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
        "\"proposal_revision\":3,\"state\":\"pending_review\"," +
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
    ref.trust_epoch = 4;
    return ref;
}

std::string Grant(const std::string& owner_domain = "owner-domain_01",
                  const std::string& manifest_digest = kManifestDigest,
                  uint32_t claim_generation = 2) {
    return std::string("{\"grant_id\":\"grant_01\",\"enrollment_id\":\"enrollment_01\",") +
        "\"device_ref\":{\"device_instance_id\":\"device_01\"," +
        "\"owner_domain_id\":\"" + owner_domain +
        "\",\"owner_domain_generation\":3,\"claim_generation\":" +
        std::to_string(claim_generation) + ",\"trust_epoch\":4}," +
        "\"manifest_ref\":{\"manifest_id\":\"manifest_01\",\"revision\":2," +
        "\"digest\":\"" + manifest_digest + "\"}," +
        "\"approval_decision_id\":\"decision_01\"," +
        "\"handoff_key_id\":\"" + kHandoffKeyId + "\"," +
        "\"operational_key_id\":\"" + kOperationalKeyId + "\"," +
        "\"issued_at\":\"2026-08-18T00:02:00Z\"," +
        "\"expires_at\":\"2026-08-18T00:12:00Z\"}";
}

std::string CollectResult(const std::string& grant = "grant_01",
                          const std::string& decision = "decision_01") {
    return std::string("{\"grant_id\":\"") + grant +
        "\",\"sealed_grant\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"," +
        "\"expires_at\":\"2026-08-18T00:10:00Z\"," +
        "\"approval_decision_id\":\"" + decision + "\"}";
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
    ClaimGrantUnsealResult UnsealClaimGrant(
        const std::string&, std::string& out,
        std::string& authenticated) override {
        if (!available) return ClaimGrantUnsealResult::WireAuthUnavailable;
        if (reject) return ClaimGrantUnsealResult::AuthenticationRejected;
        out = plaintext;
        authenticated = aad;
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

    void SetValidGrant(const EnrollmentJournalEntry& entry,
                       const std::string& grant = Grant()) {
        plaintext = grant;
        aad = DeviceClaimConsumerCore::ClaimGrantAad(entry, "grant_01", Ref());
    }

    bool available = true;
    bool reject = false;
    bool fail_destroy = false;
    int destroy_calls = 0;
    std::string plaintext;
    std::string aad;
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
        "\"enrollment_id\":\"enrollment_01\",\"grant_id\":\"grant_01\"," +
        "\"hardware_evidence_digest\":\"" + kHardwareDigest + "\"," +
        "\"manifest_digest\":\"" + kManifestDigest + "\"," +
        "\"owner_domain_generation\":3,\"owner_domain_id\":\"owner-domain_01\"," +
        "\"profile_id\":\"eidolon-trust-p256-hpke-v1\"," +
        "\"proposal_revision\":3,\"trust_epoch\":4}";
    assert(crypto.aad == expected);
    const std::string ref = DeviceClaimConsumerCore::DeviceRefJson(Ref());
    assert(ref ==
           "{\"claim_generation\":2,\"device_instance_id\":\"device_01\","
           "\"owner_domain_generation\":3,\"owner_domain_id\":\"owner-domain_01\","
           "\"trust_epoch\":4}");
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
    crypto.aad += " ";
    assert(core.AcceptCollectedGrant(CollectResult()).result ==
           DeviceClaimConsumerResult::AuthenticationRejected);
    assert(!stores.has_active);
}

void OwnerDomainBusinessOwnerManifestAndGenerationCannotBeInterchanged() {
    Stores stores;
    Crypto crypto;
    DeviceClaimConsumerCore core(stores, stores, crypto);
    Record(stores, crypto, core);
    crypto.plaintext = Grant("owner_01");  // business Owner, not Owner Domain
    crypto.aad = DeviceClaimConsumerCore::ClaimGrantAad(
        stores.enrollment, "grant_01", Ref());
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

}  // namespace

int main() {
    GoldenAadAndFiveFieldDeviceRefAreExact();
    CanonicalProposalParsingFailsClosed();
    ProposalCollectionGrantAckAndActivationResumeForwardOnly();
    ActiveClaimCommitWinsPowerLossBeforeEnrollmentCleanup();
    ActiveClaimCommitWinsPowerLossBeforeMaterialDestruction();
    ActiveClaimCannotClearAnUnrelatedEnrollmentGeneration();
    WireAuthAbsenceAndAadMutationNeverStageOrActivate();
    OwnerDomainBusinessOwnerManifestAndGenerationCannotBeInterchanged();
    DuplicateGrantAndRevokedTerminalFenceOldGeneration();
    PreActiveRevokeCannotFabricateAClaim();
    return 0;
}
