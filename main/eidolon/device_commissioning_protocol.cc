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

CommissioningReply Refusal(const char* status, const char* detail)
{
    // `detail` is always one of this file's own literals, so it needs no
    // escaping; nothing the commissioner sent is echoed back.
    return CommissioningReply{status, std::string("{\"schema_version\":1,\"error\":\"") +
                                          detail + "\"}"};
}

bool AcceptanceBody(const std::string& device_id, const CommissioningIntent& intent,
                    std::string& out)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "hub_id", intent.hub_id.c_str());
    // Named so the commissioner cannot read this answer as "already on the
    // network": when it is true, the device has not joined yet and the verdict
    // on the credentials arrives elsewhere.
    cJSON_AddBoolToObject(root, "joining_network", intent.changes_network);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return false;
    }
    out = printed;
    cJSON_free(printed);
    return true;
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

void Commission(const std::string& body, const std::string& device_id,
                const CommissioningEffects& effects)
{
    CommissioningIntent intent;
    if (!ParseCommissioningIntent(body, intent)) {
        effects.reply(Refusal("400 Bad Request", "commissioning payload is incomplete"));
        return;
    }

    // Composed before anything is stored, so a device that cannot produce an
    // answer has not yet done anything it would owe one for.
    std::string accepted;
    if (!AcceptanceBody(device_id, intent, accepted)) {
        effects.reply(Refusal("500 Internal Server Error", "out of memory"));
        return;
    }

    // Trust first: joining a network is the act of a device that already knows
    // which Host it belongs to.
    if (!effects.save_trust(intent.hub_id, intent.certificate_pem)) {
        effects.reply(Refusal("400 Bad Request", "certificate was rejected"));
        return;
    }

    // Then answer, and only then change networks. Commissioning arrives over the
    // device's own access point, so joining the commissioned network takes down
    // the very connection this answer travels on: a device that switches first
    // has done everything asked of it and told nobody.
    effects.reply(CommissioningReply{"200 OK", accepted});
    if (intent.changes_network) {
        effects.join_network(intent.ssid, intent.password);
    }
}

}  // namespace eidolon
