#include "device_commissioning_protocol.h"

#include <cJSON.h>

namespace eidolon {

namespace {

// A certificate plus Wi-Fi credentials. Anything larger is not a commissioning
// payload this firmware understands.
constexpr size_t kMaxPayloadBytes = 8 * 1024;

// A self-signed P-256 leaf is well under this.
constexpr size_t kMaxCertificateBytes = 4 * 1024;

constexpr size_t kMaxHubIdBytes = 128;
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMaxPasswordBytes = 64;

constexpr const char* kPemPrefix = "-----BEGIN CERTIFICATE-----";

std::string JsonString(const cJSON* root, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

}  // namespace

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

bool ParseCommissioningIntent(const std::string& body, CommissioningIntent& out)
{
    out = CommissioningIntent{};
    if (body.empty() || body.size() > kMaxPayloadBytes) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    // Everything is copied out of the tree before it is freed: what follows
    // must not read through a pointer into a deleted document.
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const bool schema_is_supported = cJSON_IsNumber(schema) && schema->valueint == 1;
    CommissioningIntent intent;
    intent.hub_id = JsonString(root, "hub_id");
    intent.certificate_pem = JsonString(root, "hub_certificate");

    const cJSON* wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    intent.changes_network = cJSON_IsObject(wifi);
    if (intent.changes_network) {
        intent.ssid = JsonString(wifi, "ssid");
        intent.password = JsonString(wifi, "password");
    }
    cJSON_Delete(root);

    if (!schema_is_supported) {
        return false;
    }
    if (intent.hub_id.empty() || intent.hub_id.size() > kMaxHubIdBytes) {
        return false;
    }
    if (!IsCommissionableCertificate(intent.certificate_pem)) {
        return false;
    }
    if (intent.changes_network &&
        (intent.ssid.empty() || intent.ssid.size() > kMaxSsidBytes ||
         intent.password.size() > kMaxPasswordBytes)) {
        return false;
    }

    out = intent;
    return true;
}

}  // namespace eidolon
