#include "hub_onboarding_protocol.h"

#include "eidolon_device_profile.h"

#include <cJSON.h>

#include <cstdint>
#include <set>

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

std::string Quote(const std::string& value)
{
    cJSON* string = cJSON_CreateString(value.c_str());
    char* encoded = string ? cJSON_PrintUnformatted(string) : nullptr;
    std::string result = encoded ? encoded : "";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    cJSON_Delete(string);
    return result;
}

bool Digest(const std::string& value)
{
    if (value.size() != 71 || value.rfind("sha256:", 0) != 0) {
        return false;
    }
    return value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

bool Signature(const std::string& value)
{
    return value.size() == 86 &&
           value.find_first_not_of(
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
               std::string::npos;
}

bool ExactObjectSize(const cJSON* object, int expected)
{
    return cJSON_IsObject(object) && cJSON_GetArraySize(object) == expected;
}

bool ReadInt64(const cJSON* object, const char* key, int64_t& out);

bool ReadRevision(const cJSON* object, const char* key, uint64_t& out)
{
    int64_t value = 0;
    if (!ReadInt64(object, key, value) || value < 1) {
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}

bool ParseAuthority(const std::string& value,
                    device_foundation::v1::LogicalAuthority& out)
{
    using device_foundation::v1::LogicalAuthority;
    if (value == "admission") out = LogicalAuthority::Admission;
    else if (value == "device-control") out = LogicalAuthority::DeviceControl;
    else if (value == "body-mesh") out = LogicalAuthority::BodyMesh;
    else if (value == "companion") out = LogicalAuthority::Companion;
    else return false;
    return true;
}

const char* AuthorityWire(device_foundation::v1::LogicalAuthority value)
{
    using device_foundation::v1::LogicalAuthority;
    switch (value) {
    case LogicalAuthority::Admission: return "admission";
    case LogicalAuthority::DeviceControl: return "device-control";
    case LogicalAuthority::BodyMesh: return "body-mesh";
    case LogicalAuthority::Companion: return "companion";
    }
    return "";
}

std::string CanonicalEndpoint(const device_foundation::v1::AuthorityEndpoint& value)
{
    return std::string("{\"authority\":") + Quote(AuthorityWire(value.authority)) +
           ",\"logical_audience\":" + Quote(value.logical_audience) +
           ",\"priority\":" + std::to_string(value.priority) +
           ",\"transport_profile\":" + Quote(value.transport_profile) +
           ",\"uri\":" + Quote(value.uri) + "}";
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

bool ParseOwnerDomainDescriptor(
    const std::string& body,
    device_foundation::v1::OwnerDomainDescriptor& out,
    std::string& canonical_signing_bytes)
{
    using device_foundation::v1::AuthorityEndpoint;
    using device_foundation::v1::OwnerDomainDescriptor;
    out = OwnerDomainDescriptor{};
    canonical_signing_bytes.clear();
    if (body.empty() || body.size() > 32 * 1024) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!ExactObjectSize(root, 8)) {
        cJSON_Delete(root);
        return false;
    }
    out.owner_domain_id = JsonString(root, "owner_domain_id");
    out.issued_at = JsonString(root, "issued_at");
    out.expires_at = JsonString(root, "expires_at");
    out.signing_key_id = JsonString(root, "signing_key_id");
    out.signature = JsonString(root, "signature");
    if (out.owner_domain_id.empty() || out.owner_domain_id.size() > 128 ||
        !ReadRevision(root, "directory_revision", out.directory_revision) ||
        out.issued_at.empty() || out.expires_at.empty() ||
        !Digest(out.signing_key_id) || !Signature(out.signature)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* roots = cJSON_GetObjectItemCaseSensitive(root, "trust_root_refs");
    if (!cJSON_IsArray(roots) || cJSON_GetArraySize(roots) < 1 ||
        cJSON_GetArraySize(roots) > 16) {
        cJSON_Delete(root);
        return false;
    }
    std::set<std::string> unique_roots;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, roots) {
        if (!cJSON_IsString(item) || item->valuestring == nullptr ||
            !Digest(item->valuestring) || !unique_roots.insert(item->valuestring).second) {
            cJSON_Delete(root);
            return false;
        }
        out.trust_root_refs.emplace_back(item->valuestring);
    }
    const cJSON* endpoints = cJSON_GetObjectItemCaseSensitive(root, "endpoints");
    if (!cJSON_IsArray(endpoints) || cJSON_GetArraySize(endpoints) < 1 ||
        cJSON_GetArraySize(endpoints) > 32) {
        cJSON_Delete(root);
        return false;
    }
    std::set<std::string> endpoint_ids;
    cJSON_ArrayForEach(item, endpoints) {
        AuthorityEndpoint endpoint;
        int64_t priority = -1;
        const std::string authority = JsonString(item, "authority");
        endpoint.logical_audience = JsonString(item, "logical_audience");
        endpoint.uri = JsonString(item, "uri");
        endpoint.transport_profile = JsonString(item, "transport_profile");
        if (!ExactObjectSize(item, 5) || !ParseAuthority(authority, endpoint.authority) ||
            endpoint.logical_audience.empty() || endpoint.logical_audience.size() > 128 ||
            !IsHttpsUrl(endpoint.uri) || endpoint.uri.size() > 2048 ||
            endpoint.transport_profile != "https-json" ||
            !ReadInt64(item, "priority", priority) || priority > 65535) {
            cJSON_Delete(root);
            return false;
        }
        endpoint.priority = static_cast<uint16_t>(priority);
        const std::string identity = authority + "\0" + endpoint.logical_audience +
                                     "\0" + endpoint.uri;
        if (!endpoint_ids.insert(identity).second) {
            cJSON_Delete(root);
            return false;
        }
        out.endpoints.push_back(std::move(endpoint));
    }
    cJSON_Delete(root);
    std::string canonical_endpoints = "[";
    for (size_t index = 0; index < out.endpoints.size(); ++index) {
        if (index != 0) canonical_endpoints += ',';
        canonical_endpoints += CanonicalEndpoint(out.endpoints[index]);
    }
    canonical_endpoints += ']';
    std::string canonical_roots = "[";
    for (size_t index = 0; index < out.trust_root_refs.size(); ++index) {
        if (index != 0) canonical_roots += ',';
        canonical_roots += Quote(out.trust_root_refs[index]);
    }
    canonical_roots += ']';
    canonical_signing_bytes =
        std::string("{\"directory_revision\":") +
        std::to_string(out.directory_revision) + ",\"endpoints\":" +
        canonical_endpoints + ",\"expires_at\":" + Quote(out.expires_at) +
        ",\"issued_at\":" + Quote(out.issued_at) +
        ",\"owner_domain_id\":" + Quote(out.owner_domain_id) +
        ",\"signing_key_id\":" + Quote(out.signing_key_id) +
        ",\"trust_root_refs\":" + canonical_roots + "}";
    return !canonical_signing_bytes.empty();
}

std::string BuildDeviceManifestJson(const std::string& board_name, bool has_camera)
{
    cJSON* title = cJSON_CreateString(board_name.c_str());
    char* encoded_title = title ? cJSON_PrintUnformatted(title) : nullptr;
    std::string escaped = encoded_title ? encoded_title : "\"device\"";
    if (encoded_title != nullptr) {
        cJSON_free(encoded_title);
    }
    cJSON_Delete(title);

    // The manifest is how this device tells the Host what it can carry, and the
    // Host provisions its channel from exactly this. Declaring capabilities the
    // board does not have is not cosmetic: a camera that claims a microphone is
    // granted one and is assigned a voice agent that waits forever for audio.
    // Everything below is a compile-time fact about this build, never a guess.
    std::string media = "{\"codecs\":[\"opus\"],\"direction\":\"bidirectional\",\"kind\":\"audio\"}";
    if (has_camera) {
        media += ",{\"codecs\":[\"h264\"],\"direction\":\"publish\",\"kind\":\"video\"}";
    }

    // Turn taking rides in a property whose schema pins one value: the manifest
    // carries immutable device attributes this way, and a `const` schema is the
    // device stating a fact about itself rather than offering a choice. It
    // belongs to the device because it follows from whether this build has an
    // echo-cancellation reference, which no deployment-wide setting can know.
    const char* interaction_mode =
        kModePtt ? "ptt" : (kModeHalfDuplex ? "half_duplex" : "full_duplex");
    std::string properties =
        std::string("{\"name\":\"interaction_mode\",\"observable\":false,\"schema\":{\"const\":\"") +
        interaction_mode + "\",\"type\":\"string\"},\"writable\":false}";

    // Keep a compact deterministic representation for the manifest wire
    // contract: keys stay sorted so the Host's manifest revision is stable
    // across boots that declare the same thing.
    return "{\"actions\":[],\"events\":[],\"media\":[" + media +
           "],\"properties\":[" + properties +
           "],\"schema_version\":1,\"title\":" + escaped + "}";
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
    const bool valid = JsonString(root, "operation") == "device.enrollment-received" &&
                       out.request_id == expected.request_id &&
                       out.device_id == expected.device_id &&
                       !out.enrollment_id.empty() &&
                       out.lifecycle_state == "pending-approval" &&
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
            ReadInt64(item, "expires_at_ms", assignment.expires_at_ms) &&
            assignment.expires_at_ms > 0) {
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
    const bool valid = cJSON_IsNumber(schema) && schema->valueint == 2 &&
                       ReadRoom(root, "session", out.session) && cJSON_IsObject(audio);
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

}  // namespace eidolon
