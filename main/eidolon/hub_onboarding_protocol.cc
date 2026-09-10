#include "room_config_json.h"
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

bool AsciiAlphaNumeric(unsigned char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

bool Identifier(const std::string& value)
{
    if (value.size() < 3 || value.size() > 128 ||
        !AsciiAlphaNumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const unsigned char character : value) {
        if (!AsciiAlphaNumeric(character) && character != '.' &&
            character != '_' && character != ':' && character != '-') {
            return false;
        }
    }
    return true;
}

bool OwnerDomainId(const std::string& value)
{
    return value.rfind("owner-", 0) == 0 && value.size() > 6 &&
           AsciiAlphaNumeric(static_cast<unsigned char>(value[6])) &&
           Identifier(value);
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
    return out.usable() && !out.identity.empty() && !out.room_name.empty() &&
           ReadRoomServerUrls(room, out);
}

bool ParseDeviceRef(const cJSON* item,
                    device_foundation::v1::DeviceRef& out)
{
    using device_foundation::v1::DeviceRef;
    out = DeviceRef{};
    uint64_t claim_generation = 0;
    uint64_t trust_epoch = 0;
    if (!ExactObjectSize(item, 5)) return false;
    out.device_instance_id = JsonString(item, "device_instance_id");
    out.owner_domain_id.value = JsonString(item, "owner_domain_id");
    if (!Identifier(out.device_instance_id) ||
        !OwnerDomainId(out.owner_domain_id.value) ||
        !ReadRevision(item, "owner_domain_generation",
                      out.owner_domain_generation) ||
        !ReadRevision(item, "claim_generation", claim_generation) ||
        !ReadRevision(item, "trust_epoch", trust_epoch) ||
        claim_generation > UINT32_MAX || trust_epoch > UINT32_MAX) {
        return false;
    }
    out.claim_generation = static_cast<uint32_t>(claim_generation);
    out.trust_epoch = static_cast<uint32_t>(trust_epoch);
    return true;
}

bool SameDeviceRef(const device_foundation::v1::DeviceRef& left,
                   const device_foundation::v1::DeviceRef& right)
{
    return left.device_instance_id == right.device_instance_id &&
           left.owner_domain_id.value == right.owner_domain_id.value &&
           left.owner_domain_generation == right.owner_domain_generation &&
           left.claim_generation == right.claim_generation &&
           left.trust_epoch == right.trust_epoch;
}

bool ParseOneChannel(const cJSON* root, HubChannelAssignment& assignment)
{
    assignment = HubChannelAssignment{};
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (!cJSON_IsArray(channels)) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, channels) {
        if (!cJSON_IsObject(item) ||
            JsonString(item, "binding_format") != kLiveKitBindingFormat) {
            continue;
        }
        assignment.channel_id = JsonString(item, "channel_id");
        assignment.binding_format = JsonString(item, "binding_format");
        assignment.opaque_binding = JsonString(item, "opaque_binding");
        if (!assignment.channel_id.empty() &&
            !assignment.opaque_binding.empty() &&
            ReadInt64(item, "expires_at_ms", assignment.expires_at_ms) &&
            assignment.expires_at_ms > 0) {
            return true;
        }
        assignment = HubChannelAssignment{};
    }
    return cJSON_GetArraySize(channels) == 0;
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
    if (!ExactObjectSize(root, 10)) {
        cJSON_Delete(root);
        return false;
    }
    out.owner_domain_id = JsonString(root, "owner_domain_id");
    out.descriptor_uri = JsonString(root, "descriptor_uri");
    out.issued_at = JsonString(root, "issued_at");
    out.expires_at = JsonString(root, "expires_at");
    out.signing_key_id = JsonString(root, "signing_key_id");
    out.signature = JsonString(root, "signature");
    if (!OwnerDomainId(out.owner_domain_id) ||
        !ReadRevision(root, "owner_domain_generation",
                      out.owner_domain_generation) ||
        !ReadRevision(root, "directory_revision", out.directory_revision) ||
        !IsHttpsUrl(out.descriptor_uri) || out.descriptor_uri.size() > 2048 ||
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
    // RFC 8785 orders members by their UTF-16 code units, which puts
    // "descriptor_uri" ahead of "directory_revision". The order is not a style
    // choice: the golden vector in tests/fixtures pins these exact bytes, and a
    // single misplaced member verifies as a forged signature.
    canonical_signing_bytes =
        std::string("{\"descriptor_uri\":") + Quote(out.descriptor_uri) +
        ",\"directory_revision\":" +
        std::to_string(out.directory_revision) + ",\"endpoints\":" +
        canonical_endpoints + ",\"expires_at\":" + Quote(out.expires_at) +
        ",\"issued_at\":" + Quote(out.issued_at) +
        ",\"owner_domain_generation\":" +
        std::to_string(out.owner_domain_generation) +
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

bool ParseDeviceConfigurationResponse(
    const std::string& body,
    const std::string& expected_nonce,
    const ActiveClaimState& expected,
    HubConfigStatus& status,
    HubChannelAssignment& assignment,
    AcceptedManifestRef& accepted_manifest)
{
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    device_foundation::v1::DeviceRef ref;
    const std::string lifecycle = JsonString(root, "lifecycle_state");
    const bool valid = cJSON_IsObject(root) &&
        JsonString(root, "operation") == "device-control.configuration" &&
        JsonString(root, "nonce") == expected_nonce &&
        ParseDeviceRef(cJSON_GetObjectItemCaseSensitive(root, "device_ref"), ref) &&
        SameDeviceRef(ref, expected.device_ref) &&
        ParseOneChannel(root, assignment);
    if (!valid || (lifecycle != "approved" && lifecycle != "revoked")) {
        cJSON_Delete(root);
        return false;
    }
    status = lifecycle == "revoked"
                 ? HubConfigStatus::Revoked
                 : (assignment.opaque_binding.empty()
                        ? HubConfigStatus::WaitingBinding
                        : HubConfigStatus::Active);
    // Which of this device's own declarations the Authority holds. Absent is a
    // valid answer and is reported as such, never as "it holds nothing".
    accepted_manifest = AcceptedManifestRef{};
    const cJSON* manifest = cJSON_GetObjectItemCaseSensitive(root, "manifest");
    if (cJSON_IsObject(manifest)) {
        const cJSON* revision = cJSON_GetObjectItemCaseSensitive(manifest, "revision");
        const std::string digest = JsonString(manifest, "digest");
        if (cJSON_IsNumber(revision) && revision->valueint >= 1 && !digest.empty()) {
            accepted_manifest.known = true;
            accepted_manifest.digest = digest;
            accepted_manifest.revision = revision->valueint;
        }
    }
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

bool IsFinishedProposalProblem(int status, const std::string& body)
{
    if (status != 404 && status != 410) return false;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const std::string code = JsonString(root, "code");
    const std::string authority = JsonString(root, "authority");
    cJSON_Delete(root);
    if (authority != "admission") return false;
    return code == "PROPOSAL_EXPIRED" || code == "GRANT_EXPIRED" ||
           code == "NOT_FOUND";
}

std::string CanonicalOperationalPublicKeySpki(const std::string& public_key_base64)
{
    if (public_key_base64.empty()) return {};
    if (public_key_base64.rfind("p256-spki:", 0) == 0) return public_key_base64;
    return "p256-spki:" + public_key_base64;
}

}  // namespace eidolon
