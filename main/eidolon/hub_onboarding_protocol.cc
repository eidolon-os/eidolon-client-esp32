#include "hub_onboarding_protocol.h"

#include <cJSON.h>

#include <cstdint>
#include <cstring>

namespace eidolon {

namespace {

std::string JsonString(const cJSON* object, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr
               ? item->valuestring
               : "";
}

bool IsHttpsUrl(const std::string& value)
{
    return value.rfind("https://", 0) == 0 && value.size() > 8;
}

std::string Origin(const std::string& value)
{
    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }
    const size_t path = value.find('/', scheme_end + 3);
    return value.substr(0, path == std::string::npos ? value.size() : path);
}

bool SameOrigin(const std::string& left, const std::string& right)
{
    const std::string left_origin = Origin(left);
    return !left_origin.empty() && left_origin == Origin(right);
}

bool ReadInt64(const cJSON* object, const char* key, int64_t& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble > 9007199254740991.0) {
        return false;
    }
    const int64_t value = static_cast<int64_t>(item->valuedouble);
    if (static_cast<double>(value) != item->valuedouble) {
        return false;
    }
    out = value;
    return true;
}

void AppendFramed(std::string& out, const std::string& value)
{
    out += std::to_string(value.size());
    out += ':';
    out += value;
    out += '\n';
}

bool ReadRoom(const cJSON* root, const char* key, RoomConfig& out)
{
    const cJSON* room = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(room)) {
        return false;
    }
    out.server_url = JsonString(room, "server_url");
    out.token = JsonString(room, "token");
    out.identity = JsonString(room, "identity");
    out.room_name = JsonString(room, "room_name");
    return out.usable() && !out.identity.empty() && !out.room_name.empty();
}

}  // namespace

bool ParseHubDescriptorResponse(const std::string& body,
                                const HubTxtRecord& advertised,
                                HubDescriptor& out)
{
    out = HubDescriptor{};
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON* versions = cJSON_GetObjectItemCaseSensitive(root, "protocol_versions");
    bool supports_protocol = false;
    if (cJSON_IsArray(versions)) {
        const cJSON* version = nullptr;
        cJSON_ArrayForEach(version, versions) {
            if (cJSON_IsNumber(version) &&
                version->valueint == kSupportedOnboardingProtocol) {
                supports_protocol = true;
            }
        }
    }
    out.schema_version = cJSON_IsNumber(schema) ? schema->valueint : 0;
    out.hub_id = JsonString(root, "hub_id");
    out.descriptor_uri = JsonString(root, "descriptor_uri");
    out.device_onboarding_uri = JsonString(root, "device_onboarding_uri");
    out.enrollment_uri = JsonString(root, "enrollment_uri");
    const bool valid = out.schema_version == 1 && supports_protocol &&
                       !out.hub_id.empty() &&
                       out.descriptor_uri == advertised.descriptor_uri &&
                       out.enrollment_uri == advertised.enrollment_uri &&
                       IsHttpsUrl(out.descriptor_uri) &&
                       IsHttpsUrl(out.device_onboarding_uri) &&
                       IsHttpsUrl(out.enrollment_uri) &&
                       SameOrigin(out.descriptor_uri, out.device_onboarding_uri) &&
                       SameOrigin(out.descriptor_uri, out.enrollment_uri);
    cJSON_Delete(root);
    return valid;
}

std::string BuildDeviceManifestJson(const std::string& board_name)
{
    cJSON* title = cJSON_CreateString(board_name.c_str());
    char* encoded_title = title ? cJSON_PrintUnformatted(title) : nullptr;
    std::string escaped = encoded_title ? encoded_title : "\"device\"";
    if (encoded_title != nullptr) {
        cJSON_free(encoded_title);
    }
    cJSON_Delete(title);
    // Sorted keys and compact separators are part of the signed manifest
    // revision shared with Hub's canonical JSON mapper.
    return "{\"actions\":[],\"events\":[],\"media\":[{\"codecs\":[\"opus\"],"
           "\"direction\":\"bidirectional\",\"kind\":\"audio\"}],\"properties\":[],"
           "\"schema_version\":1,\"title\":" + escaped + "}";
}

std::string BuildEnrollmentProofStatement(
    const std::string& request_id,
    const std::string& device_id,
    const std::string& retrieval_token_hash,
    const std::string& pairing_commitment,
    const std::string& device_kind,
    const std::string& display_name,
    const std::string& manifest_revision)
{
    std::string statement = "eidolon-device-enrollment-proof-v1\n";
    AppendFramed(statement, request_id);
    AppendFramed(statement, device_id);
    AppendFramed(statement, retrieval_token_hash);
    AppendFramed(statement, kPairingMethod);
    AppendFramed(statement, pairing_commitment);
    AppendFramed(statement, device_kind);
    AppendFramed(statement, display_name);
    AppendFramed(statement, manifest_revision);
    return statement;
}

bool ParseEnrollmentReceiptResponse(const std::string& body,
                                    const HubOnboardingState& expected,
                                    HubEnrollmentReceipt& out)
{
    out = HubEnrollmentReceipt{};
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    out.request_id = JsonString(root, "request_id");
    out.enrollment_id = JsonString(root, "enrollment_id");
    out.device_id = JsonString(root, "device_id");
    out.lifecycle_state = JsonString(root, "lifecycle_state");
    out.pairing_claim_uri = JsonString(root, "pairing_claim_uri");
    const bool valid = JsonString(root, "operation") == "device.enrollment-received" &&
                       out.request_id == expected.request_id &&
                       out.device_id == expected.device_id &&
                       !out.enrollment_id.empty() &&
                       out.lifecycle_state == "pending-approval" &&
                       IsHttpsUrl(out.pairing_claim_uri) &&
                       SameOrigin(expected.descriptor_uri, out.pairing_claim_uri) &&
                       ReadInt64(root, "retrieval_expires_at_ms",
                                 out.retrieval_expires_at_ms);
    cJSON_Delete(root);
    return valid;
}

bool ParseHandoffResponse(const std::string& body,
                          const std::string& expected_request_id,
                          const HubOnboardingState& state,
                          HubConfigStatus& status,
                          HubChannelAssignment& assignment)
{
    assignment = HubChannelAssignment{};
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const std::string lifecycle = JsonString(root, "lifecycle_state");
    const bool envelope_valid =
        JsonString(root, "operation") == "device.handoff-outcome" &&
        JsonString(root, "request_id") == expected_request_id &&
        JsonString(root, "enrollment_id") == state.enrollment_id &&
        JsonString(root, "device_id") == state.device_id &&
        !JsonString(root, "manifest_revision").empty();
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (!envelope_valid || !cJSON_IsArray(channels)) {
        cJSON_Delete(root);
        return false;
    }
    if (lifecycle == "pending-approval") {
        status = HubConfigStatus::PendingApproval;
        const bool empty = cJSON_GetArraySize(channels) == 0;
        cJSON_Delete(root);
        return empty;
    }
    if (lifecycle == "revoked") {
        status = HubConfigStatus::Revoked;
        cJSON_Delete(root);
        return true;
    }
    if (lifecycle != "approved") {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, channels) {
        if (!cJSON_IsObject(item) ||
            JsonString(item, "binding_format") != kLiveKitBindingFormat) {
            continue;
        }
        assignment.channel_id = JsonString(item, "channel_id");
        assignment.binding_format = JsonString(item, "binding_format");
        assignment.opaque_binding = JsonString(item, "opaque_binding");
        if (!assignment.channel_id.empty() && !assignment.opaque_binding.empty() &&
            ReadInt64(item, "expires_at_ms", assignment.expires_at_ms)) {
            break;
        }
        assignment = HubChannelAssignment{};
    }
    status = assignment.opaque_binding.empty()
                 ? HubConfigStatus::WaitingBinding
                 : HubConfigStatus::Active;
    cJSON_Delete(root);
    return true;
}

bool ParseLiveKitBinding(const std::string& body, Esp32HubConfig& out)
{
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON* audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    const bool valid = cJSON_IsNumber(schema) && schema->valueint == 1 &&
                       ReadRoom(root, "active", out.active) &&
                       ReadRoom(root, "control", out.control) &&
                       cJSON_IsObject(audio);
    if (valid) {
        const cJSON* sample_rate =
            cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
        const cJSON* channels = cJSON_GetObjectItemCaseSensitive(audio, "channels");
        if (!cJSON_IsNumber(sample_rate) || sample_rate->valueint < 8000 ||
            sample_rate->valueint > 48000 || !cJSON_IsNumber(channels) ||
            channels->valueint < 1 || channels->valueint > 2) {
            cJSON_Delete(root);
            return false;
        }
        out.sample_rate = sample_rate->valueint;
        out.channels = channels->valueint;
    }
    cJSON_Delete(root);
    return valid;
}

std::string BuildLocalPairingPayload(const HubDescriptor& descriptor,
                                     const HubOnboardingState& state)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "";
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "hub_id", descriptor.hub_id.c_str());
    cJSON_AddStringToObject(root, "enrollment_id", state.enrollment_id.c_str());
    cJSON_AddStringToObject(root, "device_id", state.device_id.c_str());
    cJSON_AddStringToObject(root, "pairing_claim_uri", state.pairing_claim_uri.c_str());
    cJSON_AddStringToObject(root, "pairing_secret", state.pairing_secret.c_str());
    cJSON_AddNumberToObject(root, "expires_at_ms",
                           static_cast<double>(state.retrieval_expires_at_ms));
    char* encoded = cJSON_PrintUnformatted(root);
    std::string payload = encoded ? encoded : "";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    return payload;
}

}  // namespace eidolon
