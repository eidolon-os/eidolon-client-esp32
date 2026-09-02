#include "device_provisioning_protocol.h"

#include <cJSON.h>

namespace eidolon {

namespace {

// The wire version of this contract. It travels as a string because every other
// Eidolon contract does, and a controller that reads a version it does not know
// must refuse rather than guess.
constexpr const char* kContractVersion = "1";
constexpr const char* kFoundationContractVersion = "1.0";
constexpr const char* kTrustProfile = "eidolon-trust-p256-hpke-v1";

// A self-signed P-256 leaf is well under this.
constexpr size_t kMaxCertificateBytes = 4 * 1024;

constexpr size_t kMaxOwnerDomainIdBytes = 128;

constexpr const char* kPemPrefix = "-----BEGIN CERTIFICATE-----";

std::string JsonString(const cJSON* root, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

bool SessionId(const std::string& value)
{
    if (value.size() < 16 || value.size() > 128) return false;
    for (const unsigned char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

const char* StatusState(
    device_foundation::v1::CommissioningStatusState state)
{
    using State = device_foundation::v1::CommissioningStatusState;
    switch (state) {
    case State::ApplyingConfiguration:
        return "applying-configuration";
    case State::Committed:
        return "committed";
    case State::RolledBack:
        return "rolled-back";
    case State::Failed:
        return "failed";
    }
    return nullptr;
}

const char* FailureCode(
    device_foundation::v1::CommissioningFailureCode code)
{
    using Code = device_foundation::v1::CommissioningFailureCode;
    switch (code) {
    case Code::None:
        return nullptr;
    case Code::NetworkRejected:
        return "NETWORK_REJECTED";
    case Code::OwnerRouteUnavailable:
        return "OWNER_ROUTE_UNAVAILABLE";
    case Code::OwnerIdentityMismatch:
        return "OWNER_IDENTITY_MISMATCH";
    case Code::StorageUnavailable:
        return "STORAGE_UNAVAILABLE";
    case Code::WindowExpired:
        return "WINDOW_EXPIRED";
    case Code::Cancelled:
        return "CANCELLED";
    case Code::Internal:
        return "INTERNAL";
    }
    return nullptr;
}

// Print and free in one place. Every builder here returns an empty string when
// the document could not be produced, and every caller treats that as a refusal
// rather than sending a half-built answer.
std::string PrintAndDelete(cJSON* root)
{
    if (root == nullptr) {
        return std::string();
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return std::string();
    }
    std::string body = printed;
    cJSON_free(printed);
    return body;
}

}  // namespace

std::string BuildSetupDescriptorJson(
    const device_foundation::v1::SetupDescriptor& descriptor)
{
    using Keys = device_foundation::v1::SetupDescriptorKeys;
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    // Written in canonical key order, so these bytes are the golden vector's
    // bytes and nothing else has to be trusted to keep the two in step.
    cJSON_AddStringToObject(root, Keys::kContractVersion,
                            device_foundation::v1::kSetupDescriptorContractVersion);
    // Canonical order puts the base identity before the instance id, and these
    // bytes are compared against the golden vector byte for byte. Stated only
    // when this device holds one: absent is the honest answer for a device that
    // has never been commissioned and for one whose storage was erased — the
    // same statement now, because nothing survives an erase to say otherwise.
    if (!descriptor.device_base_id.empty()) {
        cJSON_AddStringToObject(root, Keys::kDeviceBaseId,
                                descriptor.device_base_id.c_str());
    }
    cJSON_AddStringToObject(root, Keys::kDeviceId, descriptor.device_id.c_str());
    cJSON_AddStringToObject(root, Keys::kDeviceKind, descriptor.device_kind.c_str());
    cJSON_AddStringToObject(root, Keys::kDisplayName, descriptor.display_name.c_str());
    // Only a duration that exists is written. This layer does not decide
    // whether the offer ends — the window policy already did, and an offer with
    // no end has nothing to serialise here. The canonical duration type cannot
    // hold a sentinel, so there is no number available to write instead.
    if (descriptor.expires_in.has_value()) {
        cJSON_AddNumberToObject(root, Keys::kExpiresInSeconds,
                                descriptor.expires_in->seconds());
    }
    cJSON_AddStringToObject(root, Keys::kIdentityFingerprint,
                            descriptor.identity_fingerprint.c_str());
    cJSON_AddStringToObject(root, Keys::kSessionId, descriptor.session_id.c_str());
    cJSON_AddStringToObject(root, Keys::kTrust,
                            device_foundation::v1::SetupDescriptorTrustWireValue(
                                descriptor.trust));
    return PrintAndDelete(root);
}

bool IsCommissionableCertificate(const std::string& certificate_pem)
{
    return !certificate_pem.empty() && certificate_pem.size() < kMaxCertificateBytes &&
           certificate_pem.rfind(kPemPrefix, 0) == 0;
}

bool IsCommissionedOwnerDomain(const std::string& commissioned_owner_domain_id,
                               const std::string& discovered_owner_domain_id)
{
    return !commissioned_owner_domain_id.empty() &&
           commissioned_owner_domain_id == discovered_owner_domain_id;
}

bool ParseTrustHandover(const std::string& body, TrustHandover& out)
{
    out = TrustHandover{};
    if (body.empty() || body.size() > kMaxTrustHandoverPayloadBytes) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    // Everything is copied out of the tree before it is freed: what follows must
    // not read through a pointer into a deleted document.
    const std::string contract_version = JsonString(root, "contract_version");
    TrustHandover handover;
    handover.owner_domain_id = JsonString(root, "owner_domain_id");
    handover.owner_root_certificate_pem =
        JsonString(root, "owner_root_certificate");
    handover.authority_signing_certificate_pem =
        JsonString(root, "authority_signing_certificate");
    handover.commissioning_voucher = JsonString(root, "commissioning_voucher");
    const cJSON* descriptor =
        cJSON_GetObjectItemCaseSensitive(root, "owner_domain_descriptor");
    char* descriptor_json = cJSON_IsObject(descriptor)
                                ? cJSON_PrintUnformatted(descriptor)
                                : nullptr;
    handover.owner_domain_descriptor_json =
        descriptor_json != nullptr ? descriptor_json : "";
    if (descriptor_json != nullptr) {
        cJSON_free(descriptor_json);
    }
    cJSON_Delete(root);

    if (contract_version != kContractVersion) {
        return false;
    }
    if (handover.owner_domain_id.empty() ||
        handover.owner_domain_id.size() > kMaxOwnerDomainIdBytes ||
        handover.owner_domain_descriptor_json.empty()) {
        return false;
    }
    if (!IsCommissionableCertificate(handover.owner_root_certificate_pem) ||
        !IsCommissionableCertificate(handover.authority_signing_certificate_pem)) {
        return false;
    }

    out = handover;
    return true;
}

std::string BuildTrustStagedJson(const std::string& device_id,
                                 const std::string& owner_domain_id)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "owner_domain_id", owner_domain_id.c_str());
    cJSON_AddBoolToObject(root, "staged", true);
    return PrintAndDelete(root);
}

std::string BuildTrustRefusedJson(const char* reason)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddBoolToObject(root, "staged", false);
    // `reason` is always one of this file's callers' own literals. Nothing the
    // controller sent is echoed back.
    cJSON_AddStringToObject(root, "error", reason != nullptr ? reason : "refused");
    return PrintAndDelete(root);
}

std::string BuildCommissioningStatusJson(
    const device_foundation::v1::CommissioningStatusEvidence& evidence)
{
    using State = device_foundation::v1::CommissioningStatusState;
    const char* state = StatusState(evidence.state);
    const char* failure = FailureCode(evidence.failure_code);
    const bool committed = evidence.state == State::Committed;
    const bool failed = evidence.state == State::RolledBack ||
                        evidence.state == State::Failed;
    if (!SessionId(evidence.session_id) || evidence.setup_generation == 0 ||
        evidence.state_revision == 0 || state == nullptr ||
        (committed &&
         (!evidence.conditions.wifi_connected ||
          !evidence.conditions.owner_route_validated ||
          !evidence.conditions.trust_committed ||
          !evidence.conditions.network_committed || failure != nullptr)) ||
        (failed &&
         (failure == nullptr || evidence.conditions.trust_committed ||
          evidence.conditions.network_committed))) {
        return {};
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return {};
    cJSON_AddStringToObject(
        root, "contract", "eidolon.device-foundation.commissioning-status");
    cJSON_AddStringToObject(root, "contract_version",
                            kFoundationContractVersion);
    cJSON_AddStringToObject(root, "profile_id", kTrustProfile);
    cJSON_AddStringToObject(root, "session_id", evidence.session_id.c_str());
    cJSON_AddNumberToObject(root, "setup_generation",
                            evidence.setup_generation);
    cJSON_AddNumberToObject(root, "state_revision", evidence.state_revision);
    cJSON_AddStringToObject(root, "state", state);
    cJSON* conditions = cJSON_AddObjectToObject(root, "conditions");
    cJSON_AddBoolToObject(conditions, "wifi_connected",
                          evidence.conditions.wifi_connected);
    cJSON_AddBoolToObject(conditions, "owner_route_validated",
                          evidence.conditions.owner_route_validated);
    cJSON_AddBoolToObject(conditions, "trust_committed",
                          evidence.conditions.trust_committed);
    cJSON_AddBoolToObject(conditions, "network_committed",
                          evidence.conditions.network_committed);
    if (failure == nullptr) {
        cJSON_AddNullToObject(root, "failure_code");
    } else {
        cJSON_AddStringToObject(root, "failure_code", failure);
    }
    return PrintAndDelete(root);
}

bool ParseCommissioningTerminalAck(
    const std::string& body,
    device_foundation::v1::CommissioningTerminalAck& out)
{
    out = device_foundation::v1::CommissioningTerminalAck{};
    if (body.empty() || body.size() > 1024) return false;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 5 ||
        JsonString(root, "contract") !=
            "eidolon.device-foundation.commissioning-terminal-ack" ||
        JsonString(root, "contract_version") != kFoundationContractVersion) {
        cJSON_Delete(root);
        return false;
    }
    const std::string session_id = JsonString(root, "session_id");
    const cJSON* generation =
        cJSON_GetObjectItemCaseSensitive(root, "setup_generation");
    const cJSON* revision =
        cJSON_GetObjectItemCaseSensitive(root, "observed_state_revision");
    if (!SessionId(session_id) || !cJSON_IsNumber(generation) ||
        !cJSON_IsNumber(revision) || generation->valuedouble < 1 ||
        revision->valuedouble < 1 || generation->valuedouble > UINT32_MAX ||
        revision->valuedouble > UINT32_MAX ||
        generation->valuedouble !=
            static_cast<uint32_t>(generation->valuedouble) ||
        revision->valuedouble !=
            static_cast<uint32_t>(revision->valuedouble)) {
        cJSON_Delete(root);
        return false;
    }
    out.session_id = session_id;
    out.setup_generation = static_cast<uint32_t>(generation->valuedouble);
    out.observed_state_revision =
        static_cast<uint32_t>(revision->valuedouble);
    cJSON_Delete(root);
    return true;
}

std::string BuildEnrollmentReceiptJson(const std::string& device_id,
                                       const std::string& enrollment_id,
                                       const std::string& lifecycle_state)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    if (lifecycle_state.empty()) {
        // Not an error. The controller asked before this device had anything to
        // report, and is expected to ask again.
        cJSON_AddBoolToObject(root, "enrolled", false);
        return PrintAndDelete(root);
    }
    cJSON_AddBoolToObject(root, "enrolled", true);
    cJSON_AddStringToObject(root, "enrollment_id", enrollment_id.c_str());
    cJSON_AddStringToObject(root, "lifecycle_state", lifecycle_state.c_str());
    return PrintAndDelete(root);
}

}  // namespace eidolon
