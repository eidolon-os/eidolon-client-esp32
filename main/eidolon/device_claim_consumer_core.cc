#include "device_claim_consumer_core.h"

#include <cJSON.h>

#include <cstdio>
#include <limits>
#include <utility>

namespace eidolon {
namespace {

using device_foundation::v1::DeviceRef;
using device_foundation::v1::ClaimGrant;
using device_foundation::v1::ClaimGrantAAD;
using device_foundation::v1::ClaimGrantWireEnvelope;
using device_foundation::v1::CollectClaimGrantResult;
using device_foundation::v1::ManifestRef;
using device_foundation::v1::OwnerDomainId;

std::string Text(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool ExactObject(const cJSON* object, int size) {
    return cJSON_IsObject(object) && cJSON_GetArraySize(object) == size;
}

bool Unsigned(const cJSON* object, const char* key, uint64_t& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
        item->valuedouble > 9007199254740991.0) {
        return false;
    }
    const uint64_t value = static_cast<uint64_t>(item->valuedouble);
    if (static_cast<double>(value) != item->valuedouble) return false;
    out = value;
    return true;
}

bool Digest(const std::string& value) {
    return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
           value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

bool AsciiAlphaNumeric(unsigned char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

bool AsciiDigit(unsigned char value) {
    return value >= '0' && value <= '9';
}

bool Identifier(const std::string& value) {
    if (value.size() < 3 || value.size() > 128 ||
        !AsciiAlphaNumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!AsciiAlphaNumeric(ch) && ch != '.' && ch != '_' && ch != ':' &&
            ch != '-') {
            return false;
        }
    }
    return true;
}

bool OwnerDomain(const std::string& value) {
    return value.rfind("owner-", 0) == 0 && value.size() > 6 &&
           AsciiAlphaNumeric(static_cast<unsigned char>(value[6])) &&
           Identifier(value);
}

bool Bounded(const std::string& value, size_t minimum, size_t maximum) {
    return value.size() >= minimum && value.size() <= maximum;
}

bool Base64Url(const std::string& value, size_t minimum, size_t maximum) {
    if (!Bounded(value, minimum, maximum)) return false;
    for (const unsigned char ch : value) {
        if (!AsciiAlphaNumeric(ch) && ch != '-' && ch != '_') return false;
    }
    return true;
}

bool P256Spki(const std::string& value) {
    constexpr const char* prefix = "p256-spki:";
    if (value.rfind(prefix, 0) != 0) return false;
    const std::string encoded =
        value.substr(std::char_traits<char>::length(prefix));
    if (encoded.empty()) return false;
    for (const unsigned char ch : encoded) {
        if (!AsciiAlphaNumeric(ch) && ch != '-' && ch != '_') return false;
    }
    return true;
}

bool Rfc3339DateTime(const std::string& value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' ||
        value[16] != ':') {
        return false;
    }
    const auto digits = [&value](size_t first, size_t count) {
        for (size_t index = first; index < first + count; ++index) {
            if (!AsciiDigit(static_cast<unsigned char>(value[index]))) {
                return false;
            }
        }
        return true;
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2) ||
        !digits(11, 2) || !digits(14, 2) || !digits(17, 2)) {
        return false;
    }
    const auto number = [&value](size_t first) {
        return (value[first] - '0') * 10 + value[first + 1] - '0';
    };
    if (number(5) < 1 || number(5) > 12 || number(8) < 1 ||
        number(8) > 31 || number(11) > 23 || number(14) > 59 ||
        number(17) > 60) {
        return false;
    }
    size_t zone = 19;
    if (value[zone] == '.') {
        ++zone;
        const size_t fraction = zone;
        while (zone < value.size() &&
               AsciiDigit(static_cast<unsigned char>(value[zone]))) {
            ++zone;
        }
        if (zone == fraction) return false;
    }
    if (zone + 1 == value.size() &&
        (value[zone] == 'Z' || value[zone] == 'z')) {
        return true;
    }
    return zone + 6 == value.size() &&
           (value[zone] == '+' || value[zone] == '-') &&
           AsciiDigit(static_cast<unsigned char>(value[zone + 1])) &&
           AsciiDigit(static_cast<unsigned char>(value[zone + 2])) &&
           value[zone + 3] == ':' &&
           AsciiDigit(static_cast<unsigned char>(value[zone + 4])) &&
           AsciiDigit(static_cast<unsigned char>(value[zone + 5])) &&
           number(zone + 1) <= 23 && number(zone + 4) <= 59;
}

std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out + '"';
}

bool SameManifest(const ManifestRef& left, const ManifestRef& right) {
    return left.manifest_id == right.manifest_id &&
           left.revision == right.revision && left.digest == right.digest;
}

bool ValidClaimGrantAad(const ClaimGrantAAD& aad) {
    return device_foundation::v1::IsValid(aad) &&
           Identifier(aad.enrollment_id) &&
           Identifier(aad.device_instance_id) &&
           Digest(aad.hardware_evidence_digest) &&
           Identifier(aad.manifest_ref.manifest_id) &&
           aad.manifest_ref.revision > 0 &&
           Digest(aad.manifest_ref.digest) &&
           OwnerDomain(aad.owner_domain_id.value) &&
           Identifier(aad.grant_id);
}

bool ValidWireEnvelope(const ClaimGrantWireEnvelope& envelope) {
    return device_foundation::v1::IsValid(envelope) &&
           Digest(envelope.recipient_handoff_key_id) &&
           Base64Url(envelope.encapsulated_key, 87, 87) &&
           Base64Url(envelope.ciphertext, 22, 131072) &&
           ValidClaimGrantAad(envelope.aad);
}

bool ParseDeviceRef(const cJSON* item, DeviceRef& out) {
    out = {};
    uint64_t claim_generation = 0;
    uint64_t trust_epoch = 0;
    if (!ExactObject(item, 5) ||
        !Unsigned(item, "owner_domain_generation",
                  out.owner_domain_generation) ||
        !Unsigned(item, "claim_generation", claim_generation) ||
        !Unsigned(item, "trust_epoch", trust_epoch) ||
        claim_generation > std::numeric_limits<uint32_t>::max() ||
        trust_epoch > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out.device_instance_id = Text(item, "device_instance_id");
    out.owner_domain_id.value = Text(item, "owner_domain_id");
    out.claim_generation = static_cast<uint32_t>(claim_generation);
    out.trust_epoch = static_cast<uint32_t>(trust_epoch);
    return Identifier(out.device_instance_id) &&
           OwnerDomain(out.owner_domain_id.value);
}

DeviceClaimConsumerOutcome Result(DeviceClaimConsumerResult result,
                                  std::string payload = {}) {
    return {result, std::move(payload)};
}

}  // namespace

static bool SameClaimDeviceRef(const DeviceRef& left, const DeviceRef& right) {
    return left.device_instance_id == right.device_instance_id &&
           left.owner_domain_id.value == right.owner_domain_id.value &&
           left.owner_domain_generation == right.owner_domain_generation &&
           left.claim_generation == right.claim_generation &&
           left.trust_epoch == right.trust_epoch;
}

bool ActiveClaimState::valid() const {
    return (state == ActiveClaimLocalState::Active ||
            state == ActiveClaimLocalState::Revoked) &&
           Identifier(device_ref.device_instance_id) &&
           OwnerDomain(device_ref.owner_domain_id.value) &&
           device_ref.owner_domain_generation > 0 &&
           device_ref.claim_generation > 0 && device_ref.trust_epoch > 0 &&
           Identifier(manifest_ref.manifest_id) && manifest_ref.revision > 0 &&
           Digest(manifest_ref.digest) && Identifier(grant_id);
}

std::string DeviceClaimConsumerCore::DeviceRefJson(const DeviceRef& ref) {
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(ref.owner_domain_id.value) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

std::string DeviceClaimConsumerCore::ClaimGrantAad(const ClaimGrantAAD& aad) {
    return std::string("{\"claim_generation\":") +
           std::to_string(aad.claim_generation) +
           ",\"contract\":\"eidolon.device-foundation.claim-grant-aad\"" +
           ",\"device_instance_id\":" + Quote(aad.device_instance_id) +
           ",\"enrollment_id\":" + Quote(aad.enrollment_id) +
           ",\"grant_id\":" + Quote(aad.grant_id) +
           ",\"hardware_evidence_digest\":" +
           Quote(aad.hardware_evidence_digest) +
           ",\"manifest_ref\":{\"digest\":" +
           Quote(aad.manifest_ref.digest) +
           ",\"manifest_id\":" + Quote(aad.manifest_ref.manifest_id) +
           ",\"revision\":" + std::to_string(aad.manifest_ref.revision) + "}" +
           ",\"owner_domain_generation\":" +
           std::to_string(aad.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(aad.owner_domain_id.value) +
           ",\"profile_id\":\"eidolon-trust-p256-hpke-v1\"" +
           ",\"proposal_revision\":" +
           std::to_string(aad.proposal_revision) +
           ",\"trust_epoch\":" + std::to_string(aad.trust_epoch) + "}";
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::RecordProposal(
    const std::string& create, const std::string& result,
    uint64_t owner_domain_generation) {
    ActiveClaimState active;
    const auto active_load = active_claim_.LoadActiveClaim(active);
    if (active_load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (active_load == ClaimStoreLoadResult::Loaded) {
        return Result(active.state == ActiveClaimLocalState::Revoked
                          ? DeviceClaimConsumerResult::TerminalRevoked
                          : DeviceClaimConsumerResult::IdempotencyConflict);
    }
    cJSON* request = cJSON_ParseWithLength(create.data(), create.size());
    cJSON* response = cJSON_ParseWithLength(result.data(), result.size());
    EnrollmentJournalEntry entry;
    const cJSON* hardware = cJSON_GetObjectItemCaseSensitive(
        request, "hardware_identity_evidence");
    const cJSON* commissioning =
        cJSON_GetObjectItemCaseSensitive(request, "commissioning_proof");
    const cJSON* manifest = cJSON_GetObjectItemCaseSensitive(request, "manifest");
    const cJSON* handoff = cJSON_GetObjectItemCaseSensitive(request, "handoff_key");
    const cJSON* operational =
        cJSON_GetObjectItemCaseSensitive(request, "operational_key");
    ManifestRef requested_manifest;
    bool valid = ExactObject(request, 8) && ExactObject(response, 6) &&
        Text(request, "profile_id") == "eidolon-trust-p256-hpke-v1" &&
        ExactObject(hardware, 3) && ExactObject(manifest, 4) &&
        ExactObject(commissioning, 3) && ExactObject(handoff, 2) &&
        ExactObject(operational, 2) &&
        (Text(hardware, "scheme") == "manufacturer-p256" ||
         Text(hardware, "scheme") == "dev-self-signed-p256") &&
        Bounded(Text(hardware, "evidence"), 16, 65536) &&
        Text(commissioning, "scheme") ==
            "protocomm-security2-srp6a-aes256gcm" &&
        Bounded(Text(commissioning, "proof"), 16, 4096) &&
        Bounded(Text(commissioning, "nonce"), 16, 256) &&
        cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(manifest, "document")) &&
        Text(handoff, "scheme") == "DHKEM-P256-HKDF-SHA256" &&
        P256Spki(Text(handoff, "public_key")) &&
        Text(operational, "scheme") == "ES256-P256" &&
        P256Spki(Text(operational, "public_key")) &&
        Text(response, "state") == "pending_review" &&
        Rfc3339DateTime(Text(response, "expires_at")) &&
        Unsigned(response, "proposal_revision", entry.proposal_revision);
    requested_manifest.manifest_id = Text(manifest, "manifest_id");
    requested_manifest.digest = Text(manifest, "digest");
    valid = valid && Unsigned(manifest, "revision", requested_manifest.revision) &&
        Identifier(requested_manifest.manifest_id) &&
        Digest(requested_manifest.digest);
    entry.owner_domain_id.value = Text(request, "requested_owner_domain_id");
    entry.owner_domain_generation = owner_domain_generation;
    entry.device_instance_candidate_id =
        Text(request, "device_instance_candidate_id");
    entry.hardware_evidence_digest = Text(hardware, "evidence_digest");
    entry.manifest_ref = requested_manifest;
    entry.enrollment_id = Text(response, "enrollment_id");
    entry.collection_challenge = Text(response, "collection_challenge");
    entry.handoff_key_id = crypto_.HandoffKeyId();
    entry.operational_key_id = crypto_.OperationalKeyId();
    valid = valid && OwnerDomain(entry.owner_domain_id.value) &&
        owner_domain_generation > 0 &&
        Identifier(entry.device_instance_candidate_id) &&
        Digest(entry.hardware_evidence_digest) &&
        Identifier(entry.enrollment_id) &&
        Base64Url(entry.collection_challenge, 22, 128) &&
        Text(response, "reviewed_manifest_digest") == entry.manifest_ref.digest &&
        Digest(entry.handoff_key_id) && Digest(entry.operational_key_id);
    cJSON_Delete(request);
    cJSON_Delete(response);
    if (!valid) return Result(DeviceClaimConsumerResult::InvalidContract);

    EnrollmentJournalEntry stored;
    const auto load = enrollment_.LoadEnrollment(stored);
    if (load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (load == ClaimStoreLoadResult::Loaded) {
        const bool same = stored.enrollment_id == entry.enrollment_id &&
            stored.owner_domain_id.value == entry.owner_domain_id.value &&
            stored.owner_domain_generation == entry.owner_domain_generation &&
            stored.device_instance_candidate_id ==
                entry.device_instance_candidate_id &&
            stored.proposal_revision == entry.proposal_revision &&
            stored.collection_challenge == entry.collection_challenge &&
            SameManifest(stored.manifest_ref, entry.manifest_ref) &&
            stored.hardware_evidence_digest == entry.hardware_evidence_digest &&
            stored.handoff_key_id == entry.handoff_key_id &&
            stored.operational_key_id == entry.operational_key_id;
        return Result(same ? DeviceClaimConsumerResult::Replayed
                           : DeviceClaimConsumerResult::IdempotencyConflict);
    }
    return Result(enrollment_.StoreEnrollment(entry)
                      ? DeviceClaimConsumerResult::ProposalRecorded
                      : DeviceClaimConsumerResult::StorageFailure);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::BuildCollectionRequest() {
    EnrollmentJournalEntry entry;
    const auto load = enrollment_.LoadEnrollment(entry);
    if (load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (load == ClaimStoreLoadResult::NotFound) {
        return Result(DeviceClaimConsumerResult::NoPendingEnrollment);
    }
    if (entry.phase == EnrollmentJournalPhase::GrantStaged) {
        return AckFor(entry);
    }
    std::string proof;
    if (!crypto_.BuildHandoffKeyProof(
            entry.enrollment_id, entry.proposal_revision,
            entry.collection_challenge, proof) ||
        !Bounded(proof, 16, 4096)) {
        return Result(DeviceClaimConsumerResult::WireAuthUnavailable);
    }
    const std::string payload =
        std::string("{\"collection_challenge\":") +
        Quote(entry.collection_challenge) +
        ",\"enrollment_id\":" + Quote(entry.enrollment_id) +
        ",\"handoff_key_proof\":" + Quote(proof) +
        ",\"proposal_revision\":" +
        std::to_string(entry.proposal_revision) + "}";
    return Result(DeviceClaimConsumerResult::CollectionReady, payload);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::AcceptCollectedGrant(
    const CollectClaimGrantResult& collect_result) {
    EnrollmentJournalEntry entry;
    const auto load = enrollment_.LoadEnrollment(entry);
    if (load != ClaimStoreLoadResult::Loaded) {
        return Result(load == ClaimStoreLoadResult::StorageFailure
                          ? DeviceClaimConsumerResult::StorageFailure
                          : DeviceClaimConsumerResult::NoPendingEnrollment);
    }
    const auto& envelope = collect_result.wire_envelope;
    const auto& aad = envelope.aad;
    const std::string& grant_id = collect_result.grant_id;
    const std::string& decision = collect_result.approval_decision_id;
    if (!Identifier(grant_id) || !Identifier(decision) ||
        !Rfc3339DateTime(collect_result.expires_at) ||
        !ValidWireEnvelope(envelope) || aad.grant_id != grant_id) {
        return Result(DeviceClaimConsumerResult::InvalidContract);
    }
    if (aad.owner_domain_id.value != entry.owner_domain_id.value ||
        aad.owner_domain_generation != entry.owner_domain_generation ||
        aad.device_instance_id != entry.device_instance_candidate_id) {
        return Result(DeviceClaimConsumerResult::OwnerDomainMismatch);
    }
    if (aad.proposal_revision != entry.proposal_revision) {
        return Result(DeviceClaimConsumerResult::StaleGeneration);
    }
    if (!SameManifest(aad.manifest_ref, entry.manifest_ref)) {
        return Result(DeviceClaimConsumerResult::ManifestMismatch);
    }
    if (aad.enrollment_id != entry.enrollment_id ||
        aad.hardware_evidence_digest != entry.hardware_evidence_digest ||
        envelope.recipient_handoff_key_id != entry.handoff_key_id) {
        return Result(DeviceClaimConsumerResult::AuthenticationRejected);
    }
    if (entry.phase == EnrollmentJournalPhase::GrantStaged) {
        return Result(entry.grant_id == grant_id &&
                              entry.approval_decision_id == decision &&
                              entry.staged_device_ref.device_instance_id ==
                                  aad.device_instance_id &&
                              entry.staged_device_ref.owner_domain_id.value ==
                                  aad.owner_domain_id.value &&
                              entry.staged_device_ref.owner_domain_generation ==
                                  aad.owner_domain_generation &&
                              entry.staged_device_ref.claim_generation ==
                                  aad.claim_generation &&
                              entry.staged_device_ref.trust_epoch ==
                                  aad.trust_epoch
                          ? DeviceClaimConsumerResult::Replayed
                          : DeviceClaimConsumerResult::IdempotencyConflict);
    }
    ClaimGrant plaintext;
    const std::string canonical_aad = ClaimGrantAad(aad);
    const auto unsealed = crypto_.OpenClaimGrant(
        envelope, canonical_aad, plaintext);
    if (unsealed == ClaimGrantUnsealResult::WireAuthUnavailable) {
        return Result(DeviceClaimConsumerResult::WireAuthUnavailable);
    }
    if (unsealed != ClaimGrantUnsealResult::Authenticated) {
        return Result(DeviceClaimConsumerResult::AuthenticationRejected);
    }
    const DeviceRef& ref = plaintext.device_ref;
    const ManifestRef& manifest = plaintext.manifest_ref;
    const bool valid_plaintext = Identifier(plaintext.grant_id) &&
        Identifier(plaintext.enrollment_id) &&
        Identifier(plaintext.approval_decision_id) &&
        Digest(plaintext.handoff_key_id) &&
        Digest(plaintext.operational_key_id) &&
        Rfc3339DateTime(plaintext.issued_at) &&
        Rfc3339DateTime(plaintext.expires_at) &&
        Identifier(ref.device_instance_id) &&
        OwnerDomain(ref.owner_domain_id.value) &&
        ref.owner_domain_generation > 0 && ref.claim_generation > 0 &&
        ref.trust_epoch > 0 && Identifier(manifest.manifest_id) &&
        manifest.revision > 0 && Digest(manifest.digest);
    if (!valid_plaintext) {
        return Result(DeviceClaimConsumerResult::InvalidContract);
    }
    if (plaintext.grant_id != grant_id ||
        plaintext.enrollment_id != aad.enrollment_id ||
        plaintext.approval_decision_id != decision ||
        plaintext.handoff_key_id != envelope.recipient_handoff_key_id ||
        plaintext.handoff_key_id != entry.handoff_key_id ||
        plaintext.operational_key_id != entry.operational_key_id ||
        plaintext.expires_at != collect_result.expires_at) {
        return Result(DeviceClaimConsumerResult::AuthenticationRejected);
    }
    if (ref.device_instance_id != aad.device_instance_id ||
        ref.owner_domain_id.value != aad.owner_domain_id.value) {
        return Result(DeviceClaimConsumerResult::OwnerDomainMismatch);
    }
    if (ref.owner_domain_generation != aad.owner_domain_generation ||
        ref.claim_generation != aad.claim_generation ||
        ref.trust_epoch != aad.trust_epoch) {
        return Result(DeviceClaimConsumerResult::StaleGeneration);
    }
    if (!SameManifest(manifest, aad.manifest_ref) ||
        !SameManifest(manifest, entry.manifest_ref)) {
        return Result(DeviceClaimConsumerResult::ManifestMismatch);
    }
    entry.phase = EnrollmentJournalPhase::GrantStaged;
    entry.grant_id = grant_id;
    entry.approval_decision_id = decision;
    entry.staged_device_ref = ref;
    if (!enrollment_.StoreEnrollment(entry)) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    return Result(DeviceClaimConsumerResult::GrantStaged);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::AckFor(
    const EnrollmentJournalEntry& entry) {
    if (entry.phase != EnrollmentJournalPhase::GrantStaged ||
        entry.grant_id.empty()) {
        return Result(DeviceClaimConsumerResult::PendingReview);
    }
    std::string proof;
    if (!crypto_.BuildOperationalKeyProof(
            entry.enrollment_id, entry.grant_id,
            entry.staged_device_ref, proof) ||
        !Bounded(proof, 16, 4096)) {
        return Result(DeviceClaimConsumerResult::WireAuthUnavailable);
    }
    const std::string payload =
        std::string("{\"enrollment_id\":") + Quote(entry.enrollment_id) +
        ",\"grant_id\":" + Quote(entry.grant_id) +
        ",\"operational_key_proof\":" + Quote(proof) +
        ",\"stored_claim_generation\":" +
        std::to_string(entry.staged_device_ref.claim_generation) +
        ",\"stored_trust_epoch\":" +
        std::to_string(entry.staged_device_ref.trust_epoch) + "}";
    return Result(DeviceClaimConsumerResult::AckReady, payload);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::BuildGrantAck() {
    EnrollmentJournalEntry entry;
    const auto load = enrollment_.LoadEnrollment(entry);
    if (load != ClaimStoreLoadResult::Loaded) {
        return Result(load == ClaimStoreLoadResult::StorageFailure
                          ? DeviceClaimConsumerResult::StorageFailure
                          : DeviceClaimConsumerResult::NoPendingEnrollment);
    }
    return AckFor(entry);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::AcceptGrantAck(
    const std::string& ack_result) {
    EnrollmentJournalEntry entry;
    const auto enrollment_load = enrollment_.LoadEnrollment(entry);
    if (enrollment_load != ClaimStoreLoadResult::Loaded) {
        ActiveClaimState active;
        const auto active_load = active_claim_.LoadActiveClaim(active);
        if (active_load == ClaimStoreLoadResult::Loaded && active.valid()) {
            cJSON* root =
                cJSON_ParseWithLength(ack_result.data(), ack_result.size());
            DeviceRef replayed_ref;
            const bool replay_valid = ExactObject(root, 2) &&
                Text(root, "claim_state") == "active" &&
                ParseDeviceRef(
                    cJSON_GetObjectItemCaseSensitive(root, "device_ref"),
                    replayed_ref);
            cJSON_Delete(root);
            if (!replay_valid) {
                return Result(DeviceClaimConsumerResult::InvalidContract);
            }
            if (!SameClaimDeviceRef(replayed_ref, active.device_ref)) {
                return Result(
                    replayed_ref.device_instance_id !=
                                active.device_ref.device_instance_id ||
                            replayed_ref.owner_domain_id.value !=
                                active.device_ref.owner_domain_id.value
                        ? DeviceClaimConsumerResult::OwnerDomainMismatch
                        : DeviceClaimConsumerResult::StaleGeneration);
            }
            return Result(active.state == ActiveClaimLocalState::Revoked
                              ? DeviceClaimConsumerResult::TerminalRevoked
                              : DeviceClaimConsumerResult::Replayed);
        }
        return Result(enrollment_load == ClaimStoreLoadResult::StorageFailure ||
                              active_load == ClaimStoreLoadResult::StorageFailure
                          ? DeviceClaimConsumerResult::StorageFailure
                          : DeviceClaimConsumerResult::NoPendingEnrollment);
    }
    if (entry.phase != EnrollmentJournalPhase::GrantStaged) {
        return Result(DeviceClaimConsumerResult::PendingReview);
    }
    cJSON* root = cJSON_ParseWithLength(ack_result.data(), ack_result.size());
    DeviceRef ref;
    const bool valid = ExactObject(root, 2) &&
        Text(root, "claim_state") == "active" &&
        ParseDeviceRef(cJSON_GetObjectItemCaseSensitive(root, "device_ref"), ref);
    cJSON_Delete(root);
    if (!valid) return Result(DeviceClaimConsumerResult::InvalidContract);
    if (!SameClaimDeviceRef(ref, entry.staged_device_ref)) {
        return Result(ref.owner_domain_id.value != entry.owner_domain_id.value ||
                              ref.owner_domain_generation !=
                                  entry.owner_domain_generation
                          ? DeviceClaimConsumerResult::OwnerDomainMismatch
                          : DeviceClaimConsumerResult::StaleGeneration);
    }
    ActiveClaimState active{ref, entry.manifest_ref, entry.grant_id,
                            ActiveClaimLocalState::Active};
    if (!active_claim_.StoreActiveClaim(active)) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (!crypto_.DestroyEnrollmentMaterial(entry.enrollment_id,
                                           entry.handoff_key_id)) {
        // ActiveClaim is durable and the Enrollment checkpoint remains so
        // ResumePending can retry the idempotent key/material destruction.
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (!enrollment_.ClearEnrollment()) {
        // ActiveClaim is already durable. ResumePending performs the only safe
        // forward recovery: clear the now-terminal Enrollment checkpoint.
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    return Result(DeviceClaimConsumerResult::ClaimActivated);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::ApplyClaimRevoked(
    const DeviceRef& revoked_ref) {
    ActiveClaimState active;
    const auto active_load = active_claim_.LoadActiveClaim(active);
    if (active_load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (active_load == ClaimStoreLoadResult::Loaded) {
        if (!SameClaimDeviceRef(active.device_ref, revoked_ref)) {
            return Result(
                active.device_ref.device_instance_id !=
                            revoked_ref.device_instance_id ||
                        active.device_ref.owner_domain_id.value !=
                            revoked_ref.owner_domain_id.value
                    ? DeviceClaimConsumerResult::OwnerDomainMismatch
                    : DeviceClaimConsumerResult::StaleGeneration);
        }
        if (active.state == ActiveClaimLocalState::Revoked) {
            return Result(DeviceClaimConsumerResult::Replayed);
        }
        active.state = ActiveClaimLocalState::Revoked;
        return Result(active_claim_.StoreActiveClaim(active)
                          ? DeviceClaimConsumerResult::ClaimRevoked
                          : DeviceClaimConsumerResult::StorageFailure);
    }
    // Canonical ClaimRevoked is emitted only for Active/Suspended Claims. A
    // Proposal is a different aggregate and must never be converted into a
    // synthetic revoked Claim with a fabricated grant identity.
    return Result(DeviceClaimConsumerResult::NoPendingEnrollment);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::AbandonPendingProposal() {
    ActiveClaimState active;
    const auto active_load = active_claim_.LoadActiveClaim(active);
    if (active_load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (active_load == ClaimStoreLoadResult::Loaded) {
        return Result(DeviceClaimConsumerResult::InvalidContract);
    }
    EnrollmentJournalEntry entry;
    const auto enrollment_load = enrollment_.LoadEnrollment(entry);
    if (enrollment_load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (enrollment_load == ClaimStoreLoadResult::NotFound) {
        return Result(DeviceClaimConsumerResult::NoPendingEnrollment);
    }
    // Only destroy material that still belongs to this Proposal. If the handoff
    // key has already rotated, the material this entry named is gone and the
    // entry is the only thing left to clear.
    const bool material_is_this_proposal =
        !entry.handoff_key_id.empty() &&
        entry.handoff_key_id == crypto_.HandoffKeyId();
    if (material_is_this_proposal &&
        !crypto_.DestroyEnrollmentMaterial(entry.enrollment_id,
                                           entry.handoff_key_id)) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (!enrollment_.ClearEnrollment()) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    return Result(DeviceClaimConsumerResult::ProposalAbandoned);
}

DeviceClaimConsumerOutcome DeviceClaimConsumerCore::ResumePending() {
    ActiveClaimState active;
    const auto active_load = active_claim_.LoadActiveClaim(active);
    if (active_load == ClaimStoreLoadResult::StorageFailure) {
        return Result(DeviceClaimConsumerResult::StorageFailure);
    }
    if (active_load == ClaimStoreLoadResult::Loaded) {
        EnrollmentJournalEntry stale;
        const auto enrollment_load = enrollment_.LoadEnrollment(stale);
        if (enrollment_load == ClaimStoreLoadResult::StorageFailure) {
            return Result(DeviceClaimConsumerResult::StorageFailure);
        }
        if (enrollment_load == ClaimStoreLoadResult::Loaded) {
            const bool same_terminal_transition =
                stale.phase == EnrollmentJournalPhase::GrantStaged &&
                stale.grant_id == active.grant_id &&
                SameClaimDeviceRef(stale.staged_device_ref,
                                   active.device_ref) &&
                SameManifest(stale.manifest_ref, active.manifest_ref);
            if (!same_terminal_transition ||
                !crypto_.DestroyEnrollmentMaterial(stale.enrollment_id,
                                                   stale.handoff_key_id) ||
                !enrollment_.ClearEnrollment()) {
                return Result(DeviceClaimConsumerResult::StorageFailure);
            }
        }
        return Result(active.state == ActiveClaimLocalState::Revoked
                          ? DeviceClaimConsumerResult::TerminalRevoked
                          : DeviceClaimConsumerResult::Replayed);
    }
    return BuildCollectionRequest();
}

}  // namespace eidolon
