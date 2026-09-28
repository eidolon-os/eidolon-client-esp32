#include "eidolon/expression/generated/presentation_catalog.h"
#include "room_config_json.h"
#include "hub_onboarding_protocol.h"
#include "device_capabilities.h"

#include "eidolon_device_profile.h"
#if CONFIG_BOARD_TYPE_KORVO_1
#include "smarthome/smarthome_wire.h"
#endif

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

bool ParseChannelAssignment(const cJSON* item, HubChannelAssignment& assignment)
{
    assignment = HubChannelAssignment{};
    if (!cJSON_IsObject(item) || JsonString(item, "binding_format") != kLiveKitBindingFormat) {
        return false;
    }
    assignment.channel_id = JsonString(item, "channel_id");
    assignment.binding_format = JsonString(item, "binding_format");
    assignment.opaque_binding = JsonString(item, "opaque_binding");
    if (!assignment.channel_id.empty() && !assignment.opaque_binding.empty() &&
        ReadInt64(item, "expires_at_ms", assignment.expires_at_ms) && assignment.expires_at_ms > 0) {
        return true;
    }
    assignment = HubChannelAssignment{};
    return false;
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
        if (ParseChannelAssignment(item, assignment)) return true;
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
    auto capabilities = CompiledDeviceCapabilities();
    capabilities.camera = has_camera;
    return BuildDeviceManifestJson(board_name, capabilities);
}

std::string BuildDeviceManifestJson(const std::string& board_name, const DeviceCapabilities& capabilities)
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
    std::string media;
    if (capabilities.microphone || capabilities.speaker) {
        const char* direction = capabilities.microphone ?
            (capabilities.speaker ? "bidirectional" : "publish") : "subscribe";
        media = std::string("{\"codecs\":[\"opus\"],\"direction\":\"") + direction + "\",\"kind\":\"audio\"}";
    }
    if (capabilities.camera) {
        if (!media.empty()) media += ",";
        media += "{\"codecs\":[\"h264\"],\"direction\":\"publish\",\"kind\":\"video\"}";
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

    if (capabilities.policy_required)
        properties += ",{\"name\":\"output.contract\",\"observable\":false,\"schema\":{\"const\":\"eidolon.outputs.v1\",\"type\":\"string\"},\"writable\":false}";
    if (capabilities.expression) properties += ",{\"name\":\"expression.profile\",\"observable\":false,\"schema\":{\"const\":\"eidolon.face.v1\",\"type\":\"string\"},\"writable\":false}";
    if (capabilities.dialogue_text) properties += ",{\"name\":\"output.dialogue_text\",\"observable\":false,\"schema\":{\"const\":true,\"type\":\"boolean\"},\"writable\":false}";
    if (capabilities.speaker && capabilities.audio_cue)
        properties += ",{\"name\":\"output.audio_cue\",\"observable\":false,\"schema\":{\"const\":true,\"type\":\"boolean\"},\"writable\":false}";

    if (capabilities.motion)
        properties += ",{\"name\":\"output.motion\",\"observable\":false,\"schema\":{\"const\":true,\"type\":\"boolean\"},\"writable\":false}";

#if CONFIG_BOARD_TYPE_KORVO_1
    // Voice application is independent of the panel's display capability.
    // The Host validates this declared use case when it provisions a session.
    properties += ",{\"name\":\"voice.application\",\"observable\":false,\"schema\":{\"const\":\"home.command.v1\",\"type\":\"string\"},\"writable\":false}";
    properties += std::string(",{\"name\":\"") + smarthome::kPanelProfileProperty +
        "\",\"observable\":false,\"schema\":{\"const\":\"" +
        smarthome::kPanelProfile + "\",\"type\":\"string\"},\"writable\":false}";
#endif

    // Keep a compact deterministic representation for the manifest wire
    // contract: keys stay sorted so the Host's manifest revision is stable
    // across boots that declare the same thing.
    const std::string actions = capabilities.expression ? expression::kManifestActions : "[]";
    return "{\"actions\":" + actions + ",\"events\":[],\"media\":[" + media +
           "],\"properties\":[" + properties +
           "],\"schema_version\":1,\"title\":" + escaped + "}";
}

bool ParseDeviceConfigurationResponse(
    const std::string& body,
    const std::string& expected_nonce,
    const ActiveClaimState& expected,
    HubConfigStatus& status,
    HubChannelAssignment& assignment,
    AcceptedManifestRef& accepted_manifest,
    DeviceOutputPolicy* output_policy,
    const char** rejection_reason)
{
    if (rejection_reason) *rejection_reason = nullptr;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    // Fixed labels only: never return a nonce, identity, binding or response.
    const auto reject = [&](const char* reason) {
        if (rejection_reason) *rejection_reason = reason;
        cJSON_Delete(root);
        return false;
    };
    device_foundation::v1::DeviceRef ref;
    const std::string lifecycle = JsonString(root, "lifecycle_state");
    if (!cJSON_IsObject(root)) return reject("document");
    if (JsonString(root, "operation") != "device-control.configuration") return reject("operation");
    if (JsonString(root, "nonce") != expected_nonce) return reject("nonce");
    if (!ParseDeviceRef(cJSON_GetObjectItemCaseSensitive(root, "device_ref"), ref)) return reject("device_ref");
    if (!SameDeviceRef(ref, expected.device_ref)) return reject("lifecycle_mismatch");
    if (!ParseOneChannel(root, assignment)) return reject("channels");
    if (lifecycle != "approved" && lifecycle != "revoked") return reject("lifecycle_state");
    status = lifecycle == "revoked"
                 ? HubConfigStatus::Revoked
                 : (assignment.opaque_binding.empty()
                        ? HubConfigStatus::WaitingBinding
                        : HubConfigStatus::Active);
    // Which of this device's own declarations the Authority holds. Absent is a
    // valid answer and is reported as such, never as "it holds nothing".
    DeviceOutputPolicy policy;
    if (!ParseOutputPolicy(cJSON_GetObjectItemCaseSensitive(root,"output_policy"),policy)) {
        return reject("output_policy");
    }
    if (output_policy) *output_policy=policy;
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

bool ParseSharedSessionInvitation(const std::string& body,
                                  const ActiveClaimState& expected,
                                  int64_t now_ms,
                                  SharedSessionInvitation& out)
{
    out = SharedSessionInvitation{};
    if (now_ms <= 0 || body.empty() || body.size() > 160 * 1024) return false;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    const auto reject = [&]() { cJSON_Delete(root); return false; };
    int64_t version = 0, issued = 0;
    device_foundation::v1::DeviceRef ref;
    SharedSessionInvitation parsed;
    if (!ExactObjectSize(root, 5) || !ReadInt64(root, "schema_version", version) || version != 1 ||
        !ParseDeviceRef(cJSON_GetObjectItemCaseSensitive(root, "device_ref"), ref) ||
        !SameDeviceRef(ref, expected.device_ref)) return reject();
    parsed.session_id = JsonString(root, "session_id");
    if (parsed.session_id.empty() || parsed.session_id.size() > 64 ||
        parsed.session_id.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:-") != std::string::npos) {
        return reject();
    }
    const cJSON* channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    const cJSON* kinds = cJSON_GetObjectItemCaseSensitive(channel, "kinds");
    if (!ExactObjectSize(channel, 7) || JsonString(channel, "purpose") != "shared-session" ||
        !cJSON_IsArray(kinds) || cJSON_GetArraySize(kinds) < 1 || cJSON_GetArraySize(kinds) > 8 ||
        !ParseChannelAssignment(channel, parsed.channel) || parsed.channel.channel_id.size() > 128 ||
        parsed.channel.opaque_binding.size() > 131072 ||
        !ReadInt64(channel, "issued_at_ms", issued) || issued < 0 || issued > now_ms ||
        !ReadInt64(root, "deadline_ms", parsed.deadline_ms) || parsed.deadline_ms <= now_ms ||
        parsed.deadline_ms <= issued || parsed.deadline_ms > parsed.channel.expires_at_ms) return reject();
    const cJSON* kind = nullptr;
    cJSON_ArrayForEach(kind, kinds) {
        if (!cJSON_IsString(kind)) return reject();
    }
    cJSON_Delete(root);
    out = std::move(parsed);
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
