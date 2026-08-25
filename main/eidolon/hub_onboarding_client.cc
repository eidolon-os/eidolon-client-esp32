#include "hub_onboarding_client.h"

#include "board.h"

#include "authority_locator.h"
#include "device_identity.h"
#include "device_control_delivery_client.h"
#include "esp_idf_claim_grant_crypto.h"
#include "device_provisioning_protocol.h"
#include "hub_config_store.h"
#include "hub_onboarding_protocol.h"
#include "hub_pinned_http.h"
#include "hub_trust_store.h"
#include "rfc3339_utc.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <ctime>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#define TAG "HubOnboarding"

namespace eidolon {

namespace {

std::string Base64UrlRandom(size_t bytes)
{
    std::string random(bytes, '\0');
    esp_fill_random(random.data(), random.size());
    size_t capacity = 4 * ((bytes + 2) / 3) + 1;
    std::string encoded(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
            &written, reinterpret_cast<const unsigned char*>(random.data()),
            random.size()) != 0) {
        return "";
    }
    encoded.resize(written);
    for (char& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

bool Base64Decode(const std::string& encoded, std::string& out)
{
    size_t required = 0;
    int result = mbedtls_base64_decode(
        nullptr, 0, &required,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required == 0 ||
        required > 64 * 1024) {
        return false;
    }
    out.resize(required);
    size_t written = 0;
    result = mbedtls_base64_decode(
        reinterpret_cast<unsigned char*>(out.data()), out.size(), &written,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (result != 0) {
        out.clear();
        return false;
    }
    out.resize(written);
    return !out.empty();
}

std::string PrintJson(cJSON* root)
{
    char* encoded = root ? cJSON_PrintUnformatted(root) : nullptr;
    std::string out = encoded ? encoded : "";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    return out;
}

std::string Quote(const std::string& value)
{
    cJSON* item = cJSON_CreateString(value.c_str());
    const std::string encoded = PrintJson(item);
    cJSON_Delete(item);
    return encoded;
}

std::string DeviceRefCanonicalJson(
    const device_foundation::v1::DeviceRef& ref)
{
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(ref.owner_domain_id.value) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

std::string ConfigurationProofDocument(
    const device_foundation::v1::DeviceRef& ref,
    const std::string& nonce)
{
    return std::string("{\"device_ref\":") + DeviceRefCanonicalJson(ref) +
           ",\"nonce\":" + Quote(nonce) +
           ",\"operation_type\":\"device-control.configuration\"}";
}

cJSON* DeviceRefJson(const device_foundation::v1::DeviceRef& ref)
{
    cJSON* value = cJSON_CreateObject();
    if (!value) return nullptr;
    cJSON_AddStringToObject(value, "device_instance_id",
                            ref.device_instance_id.c_str());
    cJSON_AddStringToObject(value, "owner_domain_id",
                            ref.owner_domain_id.value.c_str());
    cJSON_AddNumberToObject(value, "owner_domain_generation",
                            static_cast<double>(ref.owner_domain_generation));
    cJSON_AddNumberToObject(value, "claim_generation", ref.claim_generation);
    cJSON_AddNumberToObject(value, "trust_epoch", ref.trust_epoch);
    return value;
}

esp_err_t StatusError(int status)
{
    if (status == 401 || status == 403) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 409 || status == 422) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == 410) {
        // Gone, not slow. Reporting a resource the Authority has finished with
        // as a timeout is how a Proposal that expired read as a network fault
        // in device logs for a whole debugging session.
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_FAIL;
}

bool HasProblemCode(const std::string& body, const char* expected)
{
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const bool matches = cJSON_IsString(code) && code->valuestring != nullptr &&
                         std::strcmp(code->valuestring, expected) == 0;
    cJSON_Delete(root);
    return matches;
}

std::string Sha256Digest(const std::string& value)
{
    unsigned char digest[32] = {};
    if (mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(value.data()), value.size(),
            digest, 0) != 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "sha256:";
    out.reserve(71);
    for (const auto byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

std::string StableToken(const std::string& material, size_t bytes)
{
    unsigned char digest[32] = {};
    if (bytes > sizeof(digest) || mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(material.data()),
            material.size(), digest, 0) != 0) return {};
    size_t capacity = 4 * ((bytes + 2) / 3) + 1;
    std::string encoded(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
            &written, digest, bytes) != 0) return {};
    encoded.resize(written);
    for (char& character : encoded) {
        if (character == '+') character = '-';
        else if (character == '/') character = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

bool JsonUint(const cJSON* object, const char* key, uint64_t& out)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 ||
        value->valuedouble > 9007199254740991.0) return false;
    out = static_cast<uint64_t>(value->valuedouble);
    return static_cast<double>(out) == value->valuedouble;
}

std::string JsonText(const cJSON* object, const char* key)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool ParseManifestRefContract(
    const cJSON* value, device_foundation::v1::ManifestRef& ref)
{
    ref = {};
    ref.manifest_id = JsonText(value, "manifest_id");
    ref.digest = JsonText(value, "digest");
    return cJSON_IsObject(value) && cJSON_GetArraySize(value) == 3 &&
        JsonUint(value, "revision", ref.revision);
}

bool ParseCollectResult(
    const std::string& body,
    device_foundation::v1::CollectClaimGrantResult& result)
{
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    const cJSON* envelope =
        cJSON_GetObjectItemCaseSensitive(root, "wire_envelope");
    const cJSON* aad = cJSON_GetObjectItemCaseSensitive(envelope, "aad");
    result = {};
    bool valid = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 4 &&
        cJSON_IsObject(envelope) && cJSON_GetArraySize(envelope) == 9 &&
        cJSON_IsObject(aad) && cJSON_GetArraySize(aad) == 12;
    if (valid) {
        result.grant_id = JsonText(root, "grant_id");
        result.expires_at = JsonText(root, "expires_at");
        result.approval_decision_id = JsonText(root, "approval_decision_id");
        auto& wire = result.wire_envelope;
        wire.contract = JsonText(envelope, "contract");
        wire.profile_id = JsonText(envelope, "profile_id");
        wire.kem = JsonText(envelope, "kem");
        wire.kdf = JsonText(envelope, "kdf");
        wire.aead = JsonText(envelope, "aead");
        wire.recipient_handoff_key_id =
            JsonText(envelope, "recipient_handoff_key_id");
        wire.encapsulated_key = JsonText(envelope, "encapsulated_key");
        wire.ciphertext = JsonText(envelope, "ciphertext");
        auto& binding = wire.aad;
        binding.contract = JsonText(aad, "contract");
        binding.profile_id = JsonText(aad, "profile_id");
        binding.enrollment_id = JsonText(aad, "enrollment_id");
        binding.device_instance_id = JsonText(aad, "device_instance_id");
        binding.hardware_evidence_digest =
            JsonText(aad, "hardware_evidence_digest");
        binding.owner_domain_id.value = JsonText(aad, "owner_domain_id");
        binding.grant_id = JsonText(aad, "grant_id");
        uint64_t claim_generation = 0;
        uint64_t trust_epoch = 0;
        valid = JsonUint(aad, "proposal_revision", binding.proposal_revision) &&
            JsonUint(aad, "owner_domain_generation",
                     binding.owner_domain_generation) &&
            JsonUint(aad, "claim_generation", claim_generation) &&
            JsonUint(aad, "trust_epoch", trust_epoch) &&
            claim_generation <= std::numeric_limits<uint32_t>::max() &&
            trust_epoch <= std::numeric_limits<uint32_t>::max() &&
            ParseManifestRefContract(
                cJSON_GetObjectItemCaseSensitive(aad, "manifest_ref"),
                binding.manifest_ref);
        if (valid) {
            binding.claim_generation = static_cast<uint32_t>(claim_generation);
            binding.trust_epoch = static_cast<uint32_t>(trust_epoch);
        }
    }
    cJSON_Delete(root);
    return valid;
}

esp_err_t AdmissionEndpoint(std::string& uri)
{
    device_foundation::v1::AuthorityEndpoint endpoint;
    const esp_err_t result = DeviceAuthorityLocator::GetInstance().Resolve(
        device_foundation::v1::LogicalAuthority::Admission, endpoint);
    if (result == ESP_OK) uri = endpoint.uri;
    return result;
}

std::string WithCommandEnvelope(const std::string& command_id,
                                const std::string& correlation_id,
                                const std::string& canonical_payload)
{
    if (canonical_payload.size() < 2 || canonical_payload.front() != '{') {
        return {};
    }
    return std::string("{\"command_id\":") + Quote(command_id) +
        ",\"correlation_id\":" + Quote(correlation_id) + "," +
        canonical_payload.substr(1);
}

}  // namespace

esp_err_t HubOnboardingClient::FetchDescriptor(
    const AuthorityCandidateRecord& candidate,
    device_foundation::v1::OwnerDomainDescriptor& out)
{
    HubHttpResponse response;
    const esp_err_t err = HubHttpRequest(
        "GET", candidate.owner_domain_descriptor_uri,
        trust_.owner_root_certificate_pem, "", response);
    if (err != ESP_OK) {
        return err;
    }
    if (response.status != 200) {
        ESP_LOGW(TAG, "Descriptor HTTP status %d", response.status);
        return StatusError(response.status);
    }
    std::string canonical;
    if (!ParseOwnerDomainDescriptor(response.body, out, canonical)) {
        ESP_LOGE(TAG, "Owner Domain descriptor is not a V1 contract document");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t accepted =
        DeviceAuthorityLocator::GetInstance().AcceptDescriptor(
            out, canonical, response.body);
    if (accepted != ESP_OK) {
        ESP_LOGE(TAG, "Owner Domain descriptor failed trust or revision checks");
        return accepted;
    }
    const esp_err_t loaded =
        DeviceAuthorityLocator::GetInstance().AcceptedDescriptor(out);
    if (loaded == ESP_OK) {
        const uint32_t revision_high =
            static_cast<uint32_t>(out.directory_revision >> 32);
        const uint32_t revision_low =
            static_cast<uint32_t>(out.directory_revision);
        if (revision_high == 0) {
            ESP_LOGI(TAG, "Accepted Owner directory owner=%s revision=%lu",
                     out.owner_domain_id.c_str(),
                     static_cast<unsigned long>(revision_low));
        } else {
            // ESP-IDF's nano formatter does not support %llu. Two fixed-width
            // halves preserve the exact 64-bit revision in device evidence.
            ESP_LOGI(TAG,
                     "Accepted Owner directory owner=%s revision=0x%08lx%08lx",
                     out.owner_domain_id.c_str(),
                     static_cast<unsigned long>(revision_high),
                     static_cast<unsigned long>(revision_low));
        }
    }
    return loaded;
}

esp_err_t HubOnboardingClient::PullActiveConfiguration(
    const ActiveClaimState& claim,
    Esp32HubConfig& out)
{
    if (!claim.valid()) return ESP_ERR_INVALID_STATE;
    auto& identity = DeviceIdentity::GetInstance();
    esp_err_t err = identity.EnsureKeypair();
    if (err != ESP_OK) return err;
    const std::string nonce = Base64UrlRandom(18);
    const std::string public_key = identity.DeviceControlPublicKey();
    std::string signature;
    err = identity.SignCanonical(
        ConfigurationProofDocument(claim.device_ref, nonce), signature);
    if (err != ESP_OK || nonce.empty() || public_key.empty() ||
        signature.size() != 86) {
        return ESP_FAIL;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* device_ref = DeviceRefJson(claim.device_ref);
    if (!root || !device_ref) {
        cJSON_Delete(root);
        cJSON_Delete(device_ref);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "device_ref", device_ref);
    cJSON_AddStringToObject(root, "nonce", nonce.c_str());
    cJSON_AddStringToObject(root, "public_key_spki", public_key.c_str());
    cJSON_AddStringToObject(root, "device_signature", signature.c_str());
    const std::string body = PrintJson(root);
    cJSON_Delete(root);
    if (body.empty()) return ESP_ERR_NO_MEM;

    device_foundation::v1::AuthorityEndpoint control;
    err = DeviceAuthorityLocator::GetInstance().Resolve(
        device_foundation::v1::LogicalAuthority::DeviceControl, control);
    if (err != ESP_OK) return err;
    HubHttpResponse response;
    err = HubHttpRequest(
        "POST", control.uri + "/configuration:pull",
        trust_.owner_root_certificate_pem, body, response);
    if (err != ESP_OK) return err;
    if (response.status != 200) return StatusError(response.status);
    HubConfigStatus status = HubConfigStatus::PendingApproval;
    HubChannelAssignment assignment;
    if (!ParseDeviceConfigurationResponse(
            response.body, nonce, claim, status, assignment)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out = Esp32HubConfig{};
    out.status = status;
    if (status == HubConfigStatus::Active) {
        std::string binding;
        if (!Base64Decode(assignment.opaque_binding, binding) ||
            !ParseLiveKitBinding(binding, out)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        out.expires_at_ms = assignment.expires_at_ms;
    }
    if (status == HubConfigStatus::Revoked) {
        ActiveClaimState revoked = claim;
        revoked.state = ActiveClaimLocalState::Revoked;
        if (!HubConfigStore().StoreActiveClaim(revoked)) {
            return ESP_FAIL;
        }
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::LoadCommissionedTrust()
{
    auto& locator = DeviceAuthorityLocator::GetInstance();
    if (locator.ReloadCommissionedDirectory() != ESP_OK ||
        locator.TrustBundle(trust_) != ESP_OK) {
        ESP_LOGE(TAG, "This device has no valid commissioned Owner Domain");
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::Run(const AuthorityCandidateRecord& candidate,
                                   const std::string& device_id,
                                   Esp32HubConfig& out)
{
    esp_err_t err = LoadCommissionedTrust();
    if (err != ESP_OK) return err;
    if (!IsCommissionedOwnerDomain(
            trust_.owner_domain_id, candidate.owner_domain_id)) {
        ESP_LOGE(TAG, "Discovery candidate is for another Owner Domain");
        return ESP_ERR_NOT_ALLOWED;
    }
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    err = FetchDescriptor(candidate, descriptor);
    if (err != ESP_OK) return err;
    return RunAccepted(descriptor, device_id, out);
}

esp_err_t HubOnboardingClient::RunAccepted(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& device_id,
    Esp32HubConfig& out)
{
    HubConfigStore store;
    ActiveClaimState active_claim;
    const ClaimStoreLoadResult claim_load =
        store.LoadActiveClaim(active_claim);
    esp_err_t err = ESP_OK;
    if (claim_load == ClaimStoreLoadResult::StorageFailure) {
        ESP_LOGE(TAG, "ActiveClaimStore is unreadable; refusing runtime");
        return ESP_FAIL;
    }
    if (claim_load == ClaimStoreLoadResult::Loaded) {
        if (active_claim.state == ActiveClaimLocalState::Revoked) {
            ESP_LOGW(TAG, "Claim is terminal revoked; physical recovery required");
            return ESP_ERR_NOT_ALLOWED;
        }
        if (active_claim.device_ref.device_instance_id != device_id ||
            active_claim.device_ref.owner_domain_id.value !=
                descriptor.owner_domain_id ||
            active_claim.device_ref.owner_domain_generation !=
                descriptor.owner_domain_generation) {
            ESP_LOGE(TAG, "Recovery required: active Claim Authority changed");
            return ESP_ERR_NOT_ALLOWED;
        }
        bool removal_completed = false;
        err = DeviceControlDeliveryClient().PollAndExecute(
            active_claim, trust_, removal_completed);
        if (err != ESP_OK) return err;
        if (removal_completed) {
            ESP_LOGW(TAG, "Device removal completed; operational runtime is fenced");
            return ESP_ERR_NOT_ALLOWED;
        }
        return PullActiveConfiguration(active_claim, out);
    }
    bool activated = false;
    err = ContinueCanonicalClaim(descriptor, device_id, active_claim, activated);
    if (err != ESP_OK) return err;
    if (!activated) {
        out = {};
        out.status = HubConfigStatus::PendingApproval;
        return ESP_OK;
    }
    return PullActiveConfiguration(active_claim, out);
}

esp_err_t HubOnboardingClient::AbandonAndRepropose(
    DeviceClaimConsumerCore& core,
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& device_id, ActiveClaimState& activated_claim,
    bool& activated, bool allow_reproposal, const char* reason)
{
    if (!allow_reproposal) {
        ESP_LOGW(TAG, "Proposal is finished (%s) and one was already abandoned",
                 reason);
        return ESP_ERR_INVALID_STATE;
    }
    const auto outcome = core.AbandonPendingProposal();
    if (outcome.result != DeviceClaimConsumerResult::ProposalAbandoned &&
        outcome.result != DeviceClaimConsumerResult::NoPendingEnrollment) {
        ESP_LOGE(TAG, "Could not abandon the finished Proposal (%s), result=%d",
                 reason, static_cast<int>(outcome.result));
        return outcome.result == DeviceClaimConsumerResult::StorageFailure
                   ? ESP_FAIL
                   : ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Proposal is finished (%s); proposing again for review",
             reason);
    return ContinueCanonicalClaim(descriptor, device_id, activated_claim,
                                  activated, false);
}

esp_err_t HubOnboardingClient::ContinueCanonicalClaim(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& device_id, ActiveClaimState& activated_claim,
    bool& activated, bool allow_reproposal)
{
    activated = false;
    HubConfigStore store;
    EspIdfClaimGrantCrypto crypto;
    if (!crypto.EnsureEnrollmentMaterial()) {
        ESP_LOGE(TAG,
                 "Claim enrollment material is absent or unreadable; refusing Proposal");
        return ESP_ERR_NOT_SUPPORTED;
    }
    DeviceClaimConsumerCore core(store, store, crypto);
    auto outcome = core.ResumePending();
    std::string admission;
    esp_err_t err = AdmissionEndpoint(admission);
    if (err != ESP_OK) return err;

    if (outcome.result == DeviceClaimConsumerResult::NoPendingEnrollment) {
#ifdef CONFIG_EIDOLON_PROVISIONING_MANUFACTURER_BOUND
        // A manufacturer-bound build must supply a real certificate-chain
        // HardwareIdentityPort. A self assertion is never a production fallback.
        return ESP_ERR_NOT_SUPPORTED;
#else
        const std::string hardware_lookup_id = SystemInfo::GetMacAddress();
        const std::string handoff_public_key = crypto.HandoffPublicKey();
        const std::string operational_public_key = crypto.OperationalPublicKey();
        const std::string nonce = StableToken(
            "commissioning|" + handoff_public_key + "|" +
                descriptor.owner_domain_id,
            18);
        std::string commissioning_proof;
        if (nonce.empty() || hardware_lookup_id.empty() ||
            !crypto.BuildDevelopmentCommissioningProof(
                hardware_lookup_id, device_id, descriptor.owner_domain_id, nonce,
                commissioning_proof)) {
            ESP_LOGE(TAG,
                     "Development Admission setup secret is not provisioned");
            return ESP_ERR_NOT_SUPPORTED;
        }
        const std::string evidence_document =
            DevelopmentHardwareEvidenceDocument(
                hardware_lookup_id, device_id, operational_public_key);
        std::string evidence_signature;
        if (evidence_document.empty() ||
            DeviceIdentity::GetInstance().SignCanonical(
                evidence_document, evidence_signature) != ESP_OK) {
            return ESP_FAIL;
        }
        const std::string hardware_evidence =
            evidence_document + "." + evidence_signature;
        const std::string hardware_digest = Sha256Digest(hardware_evidence);
        // What this board actually is. Asked of the board rather than assumed:
        // a build with no camera must not offer one, or the Host provisions a
        // video channel nobody publishes to.
        //
        // The canonical Admission cutover left a `{"endpoints":[]}` placeholder
        // here, so every device claimed through it declared nothing at all. It
        // was admitted, mounted and bound to a Companion, and then the Channel
        // Provider had nothing to provision from: the device sat in
        // WaitingBinding forever, having told the Host it could carry nothing.
        const bool has_camera = Board::GetInstance().GetCamera() != nullptr;
        const std::string manifest_document =
            BuildDeviceManifestJson(BOARD_NAME, has_camera);
        const std::string manifest_digest = Sha256Digest(manifest_document);
        const std::string canonical_create =
            std::string("{\"profile_id\":\"eidolon-trust-p256-hpke-v1\"") +
            ",\"device_instance_candidate_id\":" + Quote(device_id) +
            ",\"requested_owner_domain_id\":" +
                Quote(descriptor.owner_domain_id) +
            ",\"hardware_identity_evidence\":{" +
                std::string("\"scheme\":\"dev-self-signed-p256\",\"evidence\":") +
                Quote(hardware_evidence) + ",\"evidence_digest\":" +
                Quote(hardware_digest) + "}" +
            ",\"commissioning_proof\":{" +
                std::string("\"scheme\":\"protocomm-security2-srp6a-aes256gcm\",\"proof\":") +
                Quote(commissioning_proof) + ",\"nonce\":" + Quote(nonce) + "}" +
            ",\"manifest\":{" +
                std::string("\"manifest_id\":") + Quote(BOARD_NAME) +
                ",\"revision\":1,\"digest\":" +
                Quote(manifest_digest) + ",\"document\":" + manifest_document + "}" +
            ",\"handoff_key\":{" +
                std::string("\"scheme\":\"DHKEM-P256-HKDF-SHA256\",\"public_key\":") +
                Quote(handoff_public_key) + "}" +
            ",\"operational_key\":{" +
                std::string("\"scheme\":\"ES256-P256\",\"public_key\":") +
                Quote(operational_public_key) + "}}";
        const std::string identity = StableToken(
            "create|" + handoff_public_key + "|" +
                descriptor.owner_domain_id,
            18);
        const std::string command_id = "create-" + identity;
        const std::string correlation_id = "claim-" + identity;
        const std::string wire = WithCommandEnvelope(
            command_id, correlation_id, canonical_create);
        HubHttpResponse response;
        err = HubHttpRequest("POST", admission + "/enrollments",
                             trust_.owner_root_certificate_pem, wire, response);
        if (err != ESP_OK) return err;
        if (response.status != 201) return StatusError(response.status);
        outcome = core.RecordProposal(
            canonical_create, response.body, descriptor.owner_domain_generation);
        if (outcome.result != DeviceClaimConsumerResult::ProposalRecorded &&
            outcome.result != DeviceClaimConsumerResult::Replayed) {
            return outcome.result == DeviceClaimConsumerResult::StorageFailure
                ? ESP_FAIL
                : ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "Canonical EnrollmentProposal recorded; awaiting Decision");
        return ESP_OK;
#endif
    }

    if (outcome.result == DeviceClaimConsumerResult::CollectionReady) {
        EnrollmentJournalEntry journal;
        if (store.LoadEnrollment(journal) != ClaimStoreLoadResult::Loaded) {
            return ESP_FAIL;
        }
        const std::string identity = StableToken(
            "collect|" + journal.enrollment_id + "|" +
                journal.collection_challenge,
            18);
        HubHttpResponse response;
        err = HubHttpRequest(
            "POST",
            admission + "/enrollments/" + journal.enrollment_id +
                "/claim-grants:collect",
            trust_.owner_root_certificate_pem,
            WithCommandEnvelope("collect-" + identity, "claim-" + identity,
                                outcome.wire_payload),
            response);
        if (err != ESP_OK) return err;
        if (response.status == 409 &&
            HasProblemCode(response.body, "DECISION_REQUIRED")) {
            // A Decision may arrive after this bounded poll. Only that explicit
            // problem is retryable; revision/trust conflicts are terminal for
            // this Proposal and must never be hidden as "still pending".
            return ESP_OK;
        }
        if (IsFinishedProposalProblem(response.status, response.body)) {
            return AbandonAndRepropose(core, descriptor, device_id,
                                       activated_claim, activated,
                                       allow_reproposal, "collect");
        }
        if (response.status != 200) return StatusError(response.status);
        device_foundation::v1::CollectClaimGrantResult collected;
        if (!ParseCollectResult(response.body, collected)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        // Only a deadline this device can actually read decides anything. A
        // clock it cannot trust is the absence of an answer, and treating that
        // as expiry is how a device with no time source could collect a Grant
        // and then refuse to finish claiming, forever. The Authority refuses a
        // genuinely expired Grant on the ack, and that answer is authoritative.
        const Rfc3339DeadlineState deadline = EvaluateRfc3339Deadline(
            collected.expires_at,
            static_cast<int64_t>(time(nullptr)) * 1000,
            1704067200000LL);
        if (deadline == Rfc3339DeadlineState::Expired) {
            return AbandonAndRepropose(core, descriptor, device_id,
                                       activated_claim, activated,
                                       allow_reproposal, "grant deadline");
        }
        if (deadline == Rfc3339DeadlineState::Unknown) {
            ESP_LOGW(TAG,
                     "Grant deadline is unreadable here; letting the Authority "
                     "judge it on ack");
        }
        outcome = core.AcceptCollectedGrant(collected);
        if (outcome.result != DeviceClaimConsumerResult::GrantStaged &&
            outcome.result != DeviceClaimConsumerResult::Replayed) {
            return outcome.result == DeviceClaimConsumerResult::StorageFailure
                ? ESP_FAIL
                : ESP_ERR_INVALID_RESPONSE;
        }
        outcome = core.BuildGrantAck();
    }

    if (outcome.result == DeviceClaimConsumerResult::AckReady) {
        EnrollmentJournalEntry journal;
        if (store.LoadEnrollment(journal) != ClaimStoreLoadResult::Loaded) {
            return ESP_FAIL;
        }
        const std::string identity = StableToken(
            "ack|" + journal.enrollment_id + "|" + journal.grant_id, 18);
        HubHttpResponse response;
        err = HubHttpRequest(
            "POST",
            admission + "/enrollments/" + journal.enrollment_id +
                "/claim-grants/" + journal.grant_id + ":ack",
            trust_.owner_root_certificate_pem,
            WithCommandEnvelope("ack-" + identity, "claim-" + identity,
                                outcome.wire_payload),
            response);
        if (err != ESP_OK) return err;
        if (IsFinishedProposalProblem(response.status, response.body)) {
            return AbandonAndRepropose(core, descriptor, device_id,
                                       activated_claim, activated,
                                       allow_reproposal, "ack");
        }
        if (response.status != 200) return StatusError(response.status);
        outcome = core.AcceptGrantAck(response.body);
        if (outcome.result != DeviceClaimConsumerResult::ClaimActivated &&
            outcome.result != DeviceClaimConsumerResult::Replayed) {
            return outcome.result == DeviceClaimConsumerResult::StorageFailure
                ? ESP_FAIL
                : ESP_ERR_INVALID_RESPONSE;
        }
        if (store.LoadActiveClaim(activated_claim) !=
                ClaimStoreLoadResult::Loaded ||
            !activated_claim.valid()) {
            return ESP_FAIL;
        }
        activated = true;
        ESP_LOGI(TAG, "Canonical ClaimGrant acknowledged and Claim activated");
        return ESP_OK;
    }

    if (outcome.result == DeviceClaimConsumerResult::Replayed) {
        if (store.LoadActiveClaim(activated_claim) == ClaimStoreLoadResult::Loaded &&
            activated_claim.valid()) {
            activated = true;
            return ESP_OK;
        }
    }
    return outcome.result == DeviceClaimConsumerResult::StorageFailure
        ? ESP_FAIL
        : ESP_ERR_INVALID_STATE;
}

esp_err_t HubOnboardingClient::Resume(const std::string& device_id,
                                      Esp32HubConfig& out)
{
    esp_err_t err = LoadCommissionedTrust();
    if (err != ESP_OK) return err;
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    err = DeviceAuthorityLocator::GetInstance().AcceptedDescriptor(descriptor);
    if (err != ESP_OK) return err;
    return RunAccepted(descriptor, device_id, out);
}

}  // namespace eidolon
