#include "hub_config_client.h"

#include "board.h"
#include "device_identity.h"
#include "eidolon_topics.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>

#include <cctype>
#include <set>

#define TAG "HubConfigClient"

namespace eidolon {

namespace {

void ParseOptionalFirmware(cJSON* root, bool* has_pending, bool* force, std::string* version,
                           std::string* url) {
    *has_pending = false;
    *force = false;
    version->clear();
    url->clear();

    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (!cJSON_IsObject(firmware)) {
        return;
    }
    cJSON* ver = cJSON_GetObjectItem(firmware, "version");
    cJSON* fw_url = cJSON_GetObjectItem(firmware, "url");
    if (cJSON_IsString(ver) && cJSON_IsString(fw_url)) {
        *has_pending = true;
        *version = ver->valuestring;
        *url = fw_url->valuestring;
        cJSON* force_item = cJSON_GetObjectItem(firmware, "force");
        if (cJSON_IsNumber(force_item) && force_item->valueint == 1) {
            *force = true;
        }
        ESP_LOGI(TAG, "Hub reported firmware %s (upgrade deferred to phase 2)", version->c_str());
    }
}

#if CONFIG_EIDOLON_GUARD_SERVICE
std::string GuardRuntimeUrl(const std::string& register_url)
{
    const std::string suffix = "/api/device/register";
    const size_t pos = register_url.find(suffix);
    if (pos == std::string::npos) {
        return "";
    }
    return register_url.substr(0, pos) + "/api/guard/runtime-config";
}

std::string GuardOwnerFaceUrl(const std::string& register_url)
{
    const std::string suffix = "/api/device/register";
    const size_t pos = register_url.find(suffix);
    if (pos == std::string::npos) {
        return "";
    }
    return register_url.substr(0, pos) + "/api/guard/owner-face-profile";
}

bool IsOpaqueReferenceId(const std::string& value)
{
    if (value.empty() || value.size() > 96) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::string GuardOwnerFaceReferenceUrl(const std::string& register_url,
                                       const std::string& reference_id)
{
    std::string manifest = GuardOwnerFaceUrl(register_url);
    if (manifest.empty() || !IsOpaqueReferenceId(reference_id)) {
        return "";
    }
    const std::string suffix = "/owner-face-profile";
    return manifest.substr(0, manifest.size() - suffix.size()) +
           "/owner-face-references/" + reference_id;
}

bool IsLowerHexSha256(const std::string& value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!std::isdigit(ch) && (ch < 'a' || ch > 'f')) {
            return false;
        }
    }
    return true;
}

std::string Sha256Hex(const std::string& value)
{
    unsigned char digest[32] = {};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest, 0);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

void SetSignedGetHeaders(Http* http, const std::string& device_id,
                         const SignedRequestHeaders& signed_headers)
{
    http->SetHeader("X-Device-ID", device_id.c_str());
    http->SetHeader("X-Device-Nonce", signed_headers.nonce.c_str());
    http->SetHeader("X-Device-Timestamp", signed_headers.timestamp.c_str());
    http->SetHeader("X-Device-Public-Key", signed_headers.public_key.c_str());
    http->SetHeader("X-Device-Signature", signed_headers.signature.c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
}

bool HasExactFields(const cJSON* object, const std::set<std::string>& fields)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    std::set<std::string> seen;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        if (item->string == nullptr || fields.count(item->string) == 0 ||
            !seen.insert(item->string).second) {
            return false;
        }
    }
    return seen == fields;
}

bool ReadUnsigned(const cJSON* root, const char* key, uint32_t* out)
{
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble != static_cast<double>(item->valueint)) {
        return false;
    }
    *out = static_cast<uint32_t>(item->valueint);
    return true;
}

bool ReadRoomConfig(const cJSON* root, RoomConfig* out)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON* server_url = cJSON_GetObjectItem(root, "server_url");
    const cJSON* token = cJSON_GetObjectItem(root, "token");
    const cJSON* identity = cJSON_GetObjectItem(root, "identity");
    const cJSON* room_name = cJSON_GetObjectItem(root, "room_name");
    if (!cJSON_IsString(server_url) || !cJSON_IsString(token) || !cJSON_IsString(identity) ||
        !cJSON_IsString(room_name)) {
        return false;
    }
    out->server_url = server_url->valuestring;
    out->token = token->valuestring;
    out->identity = identity->valuestring;
    out->room_name = room_name->valuestring;
    return out->usable();
}
#endif

esp_err_t ParseEsp32ConfigResponse(const std::string& body, Esp32HubConfig& out,
                                   bool* has_pending_firmware,
                                   bool* pending_firmware_force,
                                   std::string* pending_firmware_version,
                                   std::string* pending_firmware_url)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* success = cJSON_GetObjectItem(root, "success");
    if (!cJSON_IsBool(success) || !cJSON_IsTrue(success)) {
        ESP_LOGE(TAG, "success!=true in Hub config response");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* config = cJSON_GetObjectItem(root, "config");
    if (!cJSON_IsObject(config)) {
        ESP_LOGE(TAG, "Missing config object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* status_json = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status_json)) {
        out.status = ParseHubConfigStatus(status_json->valuestring);
    }

    cJSON* server_url = cJSON_GetObjectItem(config, "server_url");
    cJSON* token = cJSON_GetObjectItem(config, "token");
    cJSON* identity = cJSON_GetObjectItem(config, "identity");
    cJSON* room_name = cJSON_GetObjectItem(config, "room_name");
    if (!cJSON_IsString(server_url) || !cJSON_IsString(token) || !cJSON_IsString(identity) ||
        !cJSON_IsString(room_name)) {
        ESP_LOGE(TAG, "Incomplete config fields");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out.active.server_url = server_url->valuestring;
    out.active.token = token->valuestring;
    out.active.identity = identity->valuestring;
    out.active.room_name = room_name->valuestring;

    cJSON* control = cJSON_GetObjectItem(config, "control");
    if (cJSON_IsObject(control)) {
        cJSON* control_server_url = cJSON_GetObjectItem(control, "server_url");
        cJSON* control_token = cJSON_GetObjectItem(control, "token");
        cJSON* control_identity = cJSON_GetObjectItem(control, "identity");
        cJSON* control_room_name = cJSON_GetObjectItem(control, "room_name");
        if (cJSON_IsString(control_server_url) && cJSON_IsString(control_token) &&
            cJSON_IsString(control_identity) && cJSON_IsString(control_room_name)) {
            out.control.server_url = control_server_url->valuestring;
            out.control.token = control_token->valuestring;
            out.control.identity = control_identity->valuestring;
            out.control.room_name = control_room_name->valuestring;
        }
    }

    cJSON* device = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsObject(device)) {
        cJSON* fingerprint = cJSON_GetObjectItem(device, "fingerprint");
        if (cJSON_IsString(fingerprint)) {
            out.device_fingerprint = fingerprint->valuestring;
        }
    }

    cJSON* audio = cJSON_GetObjectItem(config, "audio");
    if (cJSON_IsObject(audio)) {
        cJSON* sample_rate = cJSON_GetObjectItem(audio, "sample_rate");
        cJSON* channels = cJSON_GetObjectItem(audio, "channels");
        if (cJSON_IsNumber(sample_rate)) {
            out.sample_rate = sample_rate->valueint;
        }
        if (cJSON_IsNumber(channels)) {
            out.channels = channels->valueint;
        }
    }

    ParseOptionalFirmware(root, has_pending_firmware, pending_firmware_force,
                          pending_firmware_version, pending_firmware_url);
    cJSON_Delete(root);
    return ESP_OK;
}

}  // namespace

esp_err_t HubConfigClient::RegisterDevice(const std::string& register_url,
                                          const std::string& device_id,
                                          Esp32HubConfig& out,
                                          const std::string& session_intent) {
    out = Esp32HubConfig{};
    has_pending_firmware_ = false;
    pending_firmware_force_ = false;

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network interface not available");
        return ESP_ERR_INVALID_STATE;
    }

    auto http = network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_ERR_NO_MEM;
    }

    std::string request_url = register_url;
    // LiveKit agent dispatch mode stays streaming for ESP32 voice sessions.
    // Duplex/PTT capability is declared separately below via
    // X-Device-Interaction-Mode; do not use agent_mode as the barge-in switch.
    const char* agent_param = "agent_mode=streaming";
    if (request_url.find('?') != std::string::npos) {
        request_url += "&";
        request_url += agent_param;
    } else {
        request_url += "?";
        request_url += agent_param;
    }

#if CONFIG_EIDOLON_GUARD_SERVICE
    const std::string body =
        "{\"capabilities\":[{\"name\":\"device.roll_call\","
        "\"description\":\"Respond to Guard roll call with a local cue\","
        "\"input_schema\":{\"type\":\"object\",\"properties\":{},"
        "\"additionalProperties\":false},"
        "\"result_schema\":{\"type\":\"object\",\"properties\":{"
        "\"played\":{\"type\":\"boolean\"}},\"required\":[\"played\"]}}],"
        // Device identity is HARDWARE ONLY (board type/name). The logical role
        // (guard) lives on the bound companion, not the device — so it is NOT
        // encoded in the device kind. `guard` below is a protocol capability
        // declaration, not a role.
        "\"device\":{\"name\":\"" BOARD_NAME "\",\"kind\":\"" BOARD_TYPE "\"},"
        "\"guard\":true,\"guard_protocol_versions\":[1]}";
#else
    const std::string body =
        "{\"capabilities\":[{\"name\":\"device.identify\"}],"
        // Device identity is HARDWARE ONLY (board type/name); no logical role.
        "\"device\":{\"name\":\"" BOARD_NAME "\",\"kind\":\"" BOARD_TYPE "\"},"
        "\"guard\":false,\"guard_protocol_versions\":[]}";
#endif

    SignedRequestHeaders signed_headers;
    esp_err_t sign_err = DeviceIdentity::GetInstance().SignRequest(
        "POST", EidolonSignedGetPathQuery(request_url), device_id, body, signed_headers);
    if (sign_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sign Hub device registration");
        return sign_err;
    }

    http->SetHeader("X-Device-ID", device_id.c_str());
    http->SetHeader("X-Device-Nonce", signed_headers.nonce.c_str());
    http->SetHeader("X-Device-Timestamp", signed_headers.timestamp.c_str());
    http->SetHeader("X-Device-Public-Key", signed_headers.public_key.c_str());
    http->SetHeader("X-Device-Signature", signed_headers.signature.c_str());
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    // Declare the board's interaction capability so the Hub can stamp the session
    // mode into the LiveKit token metadata (Phase 4) and channel can pick the turn
    // policy (Phase 5). Hardware-determined: boards without usable AEC are PTT-only.
    // Not part of the signed canonical request — a hint, not a security artifact
    // (the authoritative per-device override is the admin path). Hub defaults to
    // half_duplex when absent, so sending it makes the device authoritative rather
    // than relying on that default.
#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
    http->SetHeader("X-Device-Interaction-Mode", kInteractionModeHalfDuplex);
#else
    http->SetHeader("X-Device-Interaction-Mode", kInteractionModeFullDuplex);
#endif
    // Why this session exists (Phase 3 proactive wake). Only sent when set, so a
    // normal JOIN omits it and the Hub defaults to user_initiated. Like the mode
    // header, it is an unsigned hint — the Hub validates it, and a bad value can
    // only ever yield a *less* surprising (non-proactive) session.
    if (!session_intent.empty()) {
        http->SetHeader("X-Device-Session-Intent", session_intent.c_str());
    }

    http->SetContent(std::string(body));
    if (!http->Open("POST", request_url)) {
        ESP_LOGE(TAG, "HTTP open failed for %s", request_url.c_str());
        return ESP_FAIL;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, register_url.c_str());
        if (status == 422) {
            return ESP_ERR_INVALID_ARG;
        }
        // 401/403 = the Hub rejected this device's signed identity (e.g. the P-256
        // key changed after a reflash, or admin revoked it). Surface it distinctly
        // so the controller can show "awaiting re-approval" instead of retrying the
        // same rejected identity forever.
        if (status == 401 || status == 403) {
            return ESP_ERR_NOT_ALLOWED;
        }
        return ESP_FAIL;
    }

    esp_err_t parse_err = ParseEsp32ConfigResponse(response, out, &has_pending_firmware_,
                                                   &pending_firmware_force_,
                                                   &pending_firmware_version_,
                                                   &pending_firmware_url_);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    ESP_LOGI(TAG, "Registered device %s status=%s", out.active.identity.c_str(),
             HubConfigStatusToString(out.status));
    return ESP_OK;
}

#if CONFIG_EIDOLON_GUARD_SERVICE
esp_err_t HubConfigClient::FetchGuardRuntime(const std::string& register_url,
                                             const std::string& device_id,
                                             GuardRuntimeHubConfig& out)
{
    out = GuardRuntimeHubConfig{};
    const std::string request_url = GuardRuntimeUrl(register_url);
    if (request_url.empty()) {
        ESP_LOGE(TAG, "Cannot derive Guard runtime URL from %s", register_url.c_str());
        return ESP_ERR_INVALID_ARG;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return ESP_ERR_INVALID_STATE;
    }
    auto http = network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS);
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    SignedRequestHeaders signed_headers;
    esp_err_t sign_err = DeviceIdentity::GetInstance().SignGetRequest(
        EidolonSignedGetPathQuery(request_url), device_id, signed_headers);
    if (sign_err != ESP_OK) {
        return sign_err;
    }
    http->SetHeader("X-Device-ID", device_id.c_str());
    http->SetHeader("X-Device-Nonce", signed_headers.nonce.c_str());
    http->SetHeader("X-Device-Timestamp", signed_headers.timestamp.c_str());
    http->SetHeader("X-Device-Public-Key", signed_headers.public_key.c_str());
    http->SetHeader("X-Device-Signature", signed_headers.signature.c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    if (!http->Open("GET", request_url)) {
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status == 409) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 401 || status == 403) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Guard runtime config HTTP status %d", status);
        return status == 422 ? ESP_ERR_INVALID_ARG : ESP_FAIL;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* success = cJSON_GetObjectItem(root, "success");
    const cJSON* schema_v = cJSON_GetObjectItem(root, "schema_v");
    const cJSON* binding_id = cJSON_GetObjectItem(root, "binding_id");
    const cJSON* guard_companion_id = cJSON_GetObjectItem(root, "guard_companion_id");
    const cJSON* desired = cJSON_GetObjectItem(root, "desired_runtime_state");
    const cJSON* runtime = cJSON_GetObjectItem(root, "runtime_config");
    const cJSON* control = cJSON_GetObjectItem(root, "control");
    bool valid = cJSON_IsTrue(success) && cJSON_IsNumber(schema_v) && schema_v->valueint == 1 &&
                 cJSON_IsString(binding_id) && cJSON_IsString(guard_companion_id) &&
                 cJSON_IsString(desired) && cJSON_IsObject(runtime) &&
                 ReadUnsigned(root, "runtime_revision", &out.runtime_revision) &&
                 ReadUnsigned(runtime, "sample_interval_ms", &out.sample_interval_ms) &&
                 ReadUnsigned(runtime, "preview_interval_ms", &out.preview_interval_ms) &&
                 ReadUnsigned(runtime, "motion_threshold", &out.motion_threshold) &&
                 ReadUnsigned(runtime, "motion_clear_threshold", &out.motion_clear_threshold) &&
                 ReadUnsigned(runtime, "candidate_debounce_ms", &out.candidate_debounce_ms) &&
                 ReadUnsigned(runtime, "absence_timeout_ms", &out.absence_timeout_ms) &&
                 ReadUnsigned(runtime, "consecutive_capture_failures", &out.consecutive_capture_failures) &&
                 ReadRoomConfig(control, &out.control);
    if (valid) {
        const cJSON* runtime_schema = cJSON_GetObjectItem(runtime, "schema_v");
        valid = cJSON_IsNumber(runtime_schema) && runtime_schema->valueint == 1 &&
                (std::string(desired->valuestring) == "running" ||
                 std::string(desired->valuestring) == "stopped");
    }
    if (valid) {
        out.binding_id = binding_id->valuestring;
        out.guard_companion_id = guard_companion_id->valuestring;
        out.desired_runtime_state = desired->valuestring;
        valid = !out.binding_id.empty() && !out.guard_companion_id.empty() &&
                out.runtime_revision > 0 &&
                out.preview_interval_ms >= out.sample_interval_ms &&
                out.motion_clear_threshold <= out.motion_threshold &&
                out.candidate_debounce_ms >= out.sample_interval_ms &&
                out.absence_timeout_ms >= out.sample_interval_ms * 2U;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t HubConfigClient::FetchOwnerFaceProfile(const std::string& config_url,
                                                 const std::string& device_id,
                                                 OwnerFaceProfileHubConfig& out)
{
    out = OwnerFaceProfileHubConfig{};
    const std::string request_url = GuardOwnerFaceUrl(config_url);
    if (request_url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS) : nullptr;
    if (!http) {
        return network ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    SignedRequestHeaders signed_headers;
    esp_err_t err = DeviceIdentity::GetInstance().SignGetRequest(
        EidolonSignedGetPathQuery(request_url), device_id, signed_headers);
    if (err != ESP_OK) {
        return err;
    }
    SetSignedGetHeaders(http.get(), device_id, signed_headers);
    if (!http->Open("GET", request_url)) {
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 401 || status == 403) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Owner Face manifest HTTP status %d", status);
        return status == 422 ? ESP_ERR_INVALID_ARG : ESP_FAIL;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    static const std::set<std::string> kManifestFields = {
        "schema_v", "binding_id", "profile_id", "profile_revision", "desired_state",
        "model_id", "preprocessing_version", "references",
    };
    static const std::set<std::string> kReferenceFields = {
        "reference_id", "pose", "sha256", "size_bytes", "content_type",
    };
    const cJSON* schema = cJSON_GetObjectItem(root, "schema_v");
    const cJSON* binding = cJSON_GetObjectItem(root, "binding_id");
    const cJSON* profile = cJSON_GetObjectItem(root, "profile_id");
    const cJSON* desired = cJSON_GetObjectItem(root, "desired_state");
    const cJSON* model = cJSON_GetObjectItem(root, "model_id");
    const cJSON* preprocessing = cJSON_GetObjectItem(root, "preprocessing_version");
    const cJSON* references = cJSON_GetObjectItem(root, "references");
    bool valid = HasExactFields(root, kManifestFields) && cJSON_IsNumber(schema) &&
                 schema->valueint == 1 && cJSON_IsString(binding) && binding->valuestring &&
                 cJSON_IsString(profile) && profile->valuestring && cJSON_IsString(desired) &&
                 desired->valuestring && cJSON_IsArray(references) &&
                 ReadUnsigned(root, "profile_revision", &out.profile_revision);
    if (valid) {
        out.binding_id = binding->valuestring;
        out.profile_id = profile->valuestring;
        out.desired_state = desired->valuestring;
        valid = !out.binding_id.empty() && !out.profile_id.empty() && out.profile_revision > 0 &&
                (out.desired_state == "active" || out.desired_state == "cleared");
    }
    std::set<std::string> reference_ids;
    std::set<std::string> poses;
    if (valid && out.desired_state == "active") {
        valid = cJSON_IsString(model) && model->valuestring &&
                cJSON_IsString(preprocessing) && preprocessing->valuestring;
        if (valid) {
            out.model_id = model->valuestring;
            out.preprocessing_version = preprocessing->valuestring;
            const int count = cJSON_GetArraySize(references);
            valid = !out.model_id.empty() && !out.preprocessing_version.empty() &&
                    count >= 3 && count <= 5;
            for (int i = 0; valid && i < count; ++i) {
                const cJSON* item = cJSON_GetArrayItem(references, i);
                const cJSON* reference_id = cJSON_GetObjectItem(item, "reference_id");
                const cJSON* pose = cJSON_GetObjectItem(item, "pose");
                const cJSON* sha256 = cJSON_GetObjectItem(item, "sha256");
                const cJSON* content_type = cJSON_GetObjectItem(item, "content_type");
                OwnerFaceReferenceHubConfig parsed;
                valid = HasExactFields(item, kReferenceFields) &&
                        cJSON_IsString(reference_id) && reference_id->valuestring &&
                        cJSON_IsString(pose) && pose->valuestring &&
                        cJSON_IsString(sha256) && sha256->valuestring &&
                        cJSON_IsString(content_type) && content_type->valuestring &&
                        ReadUnsigned(item, "size_bytes", &parsed.size_bytes);
                if (!valid) {
                    break;
                }
                parsed.reference_id = reference_id->valuestring;
                parsed.pose = pose->valuestring;
                parsed.sha256 = sha256->valuestring;
                parsed.content_type = content_type->valuestring;
                valid = IsOpaqueReferenceId(parsed.reference_id) &&
                        (parsed.pose == "front" || parsed.pose == "left" ||
                         parsed.pose == "right" || parsed.pose == "down" ||
                         parsed.pose == "up") && IsLowerHexSha256(parsed.sha256) &&
                        parsed.size_bytes > 0 && parsed.size_bytes <= 4 * 1024 * 1024 &&
                        parsed.content_type == "image/jpeg" &&
                        reference_ids.insert(parsed.reference_id).second &&
                        poses.insert(parsed.pose).second;
                if (valid) {
                    out.references.push_back(std::move(parsed));
                }
            }
            valid = valid && poses.count("front") && poses.count("left") && poses.count("right");
        }
    } else if (valid) {
        valid = cJSON_IsNull(model) && cJSON_IsNull(preprocessing) &&
                cJSON_GetArraySize(references) == 0;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t HubConfigClient::FetchOwnerFaceReference(
    const std::string& config_url, const std::string& device_id,
    const OwnerFaceReferenceHubConfig& reference, std::string& out)
{
    out.clear();
    if (!IsOpaqueReferenceId(reference.reference_id) ||
        !IsLowerHexSha256(reference.sha256) || reference.size_bytes == 0 ||
        reference.size_bytes > 4 * 1024 * 1024 || reference.content_type != "image/jpeg") {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string request_url =
        GuardOwnerFaceReferenceUrl(config_url, reference.reference_id);
    if (request_url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS) : nullptr;
    if (!http) {
        return network ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    SignedRequestHeaders signed_headers;
    esp_err_t err = DeviceIdentity::GetInstance().SignGetRequest(
        EidolonSignedGetPathQuery(request_url), device_id, signed_headers);
    if (err != ESP_OK) {
        return err;
    }
    SetSignedGetHeaders(http.get(), device_id, signed_headers);
    http->SetHeader("Accept", "image/jpeg");
    if (!http->Open("GET", request_url)) {
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    if (status == 200 && http->GetBodyLength() > 4 * 1024 * 1024) {
        http->Close();
        return ESP_ERR_INVALID_SIZE;
    }
    out = http->ReadAll();
    http->Close();
    if (status == 404) {
        out.clear();
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 401 || status == 403) {
        out.clear();
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200) {
        out.clear();
        return ESP_FAIL;
    }
    if (out.size() != reference.size_bytes || Sha256Hex(out) != reference.sha256) {
        out.clear();
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
#endif

}  // namespace eidolon
