#include "device_provisioning_protocol.h"

#include <cJSON.h>

namespace eidolon {

namespace {

// The wire version of this contract. It travels as a string because every other
// Eidolon contract does, and a controller that reads a version it does not know
// must refuse rather than guess.
constexpr const char* kContractVersion = "1";

// A certificate plus a Host id. Anything larger is not a handover this firmware
// understands.
constexpr size_t kMaxPayloadBytes = 8 * 1024;

// A self-signed P-256 leaf is well under this.
constexpr size_t kMaxCertificateBytes = 4 * 1024;

constexpr size_t kMaxHubIdBytes = 128;

constexpr const char* kPemPrefix = "-----BEGIN CERTIFICATE-----";

// The two values the Eidolon OS device abstraction admits for descriptor trust.
// A development device is discovered without a manufacturer-bound identity; a
// production device proves one. Nothing else about the act changes.
constexpr const char* kTrustDevelopmentTofu = "development-tofu";
constexpr const char* kTrustManufacturerBound = "manufacturer-bound";

std::string JsonString(const cJSON* root, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
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

std::string BuildProvisioningDescriptorJson(const ProvisioningDescriptor& descriptor)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddStringToObject(root, "device_id", descriptor.device_id.c_str());
    cJSON_AddStringToObject(root, "device_kind", descriptor.device_kind.c_str());
    cJSON_AddStringToObject(root, "display_name", descriptor.display_name.c_str());
    cJSON_AddStringToObject(root, "identity_fingerprint",
                            descriptor.identity_fingerprint.c_str());
    cJSON_AddStringToObject(root, "session_id", descriptor.session_id.c_str());
    cJSON_AddNumberToObject(root, "expires_in_seconds", descriptor.expires_in_seconds);
    cJSON_AddStringToObject(root, "trust",
                            descriptor.manufacturer_bound ? kTrustManufacturerBound
                                                          : kTrustDevelopmentTofu);
    return PrintAndDelete(root);
}

bool IsCommissionableCertificate(const std::string& certificate_pem)
{
    return !certificate_pem.empty() && certificate_pem.size() < kMaxCertificateBytes &&
           certificate_pem.rfind(kPemPrefix, 0) == 0;
}

bool IsCommissionedHub(const std::string& commissioned_hub_id,
                       const std::string& discovered_hub_id)
{
    return !commissioned_hub_id.empty() && commissioned_hub_id == discovered_hub_id;
}

bool ParseTrustHandover(const std::string& body, TrustHandover& out)
{
    out = TrustHandover{};
    if (body.empty() || body.size() > kMaxPayloadBytes) {
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
    handover.hub_id = JsonString(root, "hub_id");
    handover.certificate_pem = JsonString(root, "hub_certificate");
    cJSON_Delete(root);

    if (contract_version != kContractVersion) {
        return false;
    }
    if (handover.hub_id.empty() || handover.hub_id.size() > kMaxHubIdBytes) {
        return false;
    }
    if (!IsCommissionableCertificate(handover.certificate_pem)) {
        return false;
    }

    out = handover;
    return true;
}

std::string BuildTrustAcceptedJson(const std::string& device_id, const std::string& hub_id)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "hub_id", hub_id.c_str());
    cJSON_AddBoolToObject(root, "accepted", true);
    return PrintAndDelete(root);
}

std::string BuildTrustRefusedJson(const char* reason)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }
    cJSON_AddStringToObject(root, "contract_version", kContractVersion);
    cJSON_AddBoolToObject(root, "accepted", false);
    // `reason` is always one of this file's callers' own literals. Nothing the
    // controller sent is echoed back.
    cJSON_AddStringToObject(root, "error", reason != nullptr ? reason : "refused");
    return PrintAndDelete(root);
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
