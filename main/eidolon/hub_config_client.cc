#include "hub_config_client.h"

#include "board.h"
#include "device_identity.h"
#include "eidolon_device_profile.h"
#include "eidolon_topics.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <sys/time.h>
#include <time.h>

#define TAG "HubConfigClient"

namespace eidolon {

namespace {

#if CONFIG_EIDOLON_GUARD_SERVICE
std::string GuardRuntimeUrl(const std::string& descriptor_uri)
{
    const std::string suffix = "/api/device-onboarding/v1/descriptor";
    const size_t pos = descriptor_uri.find(suffix);
    if (pos == std::string::npos) {
        return "";
    }
    return descriptor_uri.substr(0, pos) + "/api/guard/runtime-config";
}

std::string GuardOwnerFaceUrl(const std::string& descriptor_uri)
{
    const std::string suffix = "/api/device-onboarding/v1/descriptor";
    const size_t pos = descriptor_uri.find(suffix);
    if (pos == std::string::npos) {
        return "";
    }
    return descriptor_uri.substr(0, pos) + "/api/guard/owner-face-profile";
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

std::string GuardOwnerFaceReferenceUrl(const std::string& descriptor_uri,
                                       const std::string& reference_id)
{
    std::string manifest = GuardOwnerFaceUrl(descriptor_uri);
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

}  // namespace

#if CONFIG_EIDOLON_GUARD_SERVICE
esp_err_t HubConfigClient::FetchGuardRuntime(const std::string& descriptor_uri,
                                             const std::string& device_id,
                                             GuardRuntimeHubConfig& out)
{
    out = GuardRuntimeHubConfig{};
    const std::string request_url = GuardRuntimeUrl(descriptor_uri);
    if (request_url.empty()) {
        ESP_LOGE(TAG, "Cannot derive Guard runtime URL from %s", descriptor_uri.c_str());
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
                 ReadUnsigned(runtime, "owner_face_interval_ms", &out.owner_face_interval_ms) &&
                 ReadUnsigned(runtime, "owner_presence_enter_ms", &out.owner_presence_enter_ms) &&
                 ReadUnsigned(runtime, "owner_presence_exit_ms", &out.owner_presence_exit_ms) &&
                 ReadUnsigned(runtime, "owner_presence_heartbeat_ms", &out.owner_presence_heartbeat_ms) &&
                 ReadUnsigned(runtime, "owner_presence_lease_ms", &out.owner_presence_lease_ms) &&
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
                out.absence_timeout_ms >= out.sample_interval_ms * 2U &&
                out.owner_presence_enter_ms >= out.owner_face_interval_ms &&
                out.owner_presence_exit_ms >= out.owner_face_interval_ms * 2U &&
                out.owner_presence_heartbeat_ms < out.owner_presence_lease_ms;
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
    const size_t content_length = http->GetBodyLength();
    ESP_LOGI(
        TAG,
        "Owner Face reference HTTP status=%d content_length=%lu expected=%lu "
        "internal=%lu largest_internal=%lu stack_free=%lu",
        status, static_cast<unsigned long>(content_length),
        static_cast<unsigned long>(reference.size_bytes),
        static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    if (status == 200 &&
        (content_length != reference.size_bytes ||
         content_length > 4 * 1024 * 1024)) {
        http->Close();
        return ESP_ERR_INVALID_SIZE;
    }
    if (status == 200) {
        out.reserve(reference.size_bytes);
        char buffer[512];
        while (out.size() < reference.size_bytes) {
            const size_t remaining = reference.size_bytes - out.size();
            const int received = http->Read(buffer, std::min(remaining, sizeof(buffer)));
            if (received <= 0) {
                ESP_LOGE(
                    TAG,
                    "Owner Face reference read failed received=%lu expected=%lu "
                    "last_error=%d internal=%lu largest_internal=%lu stack_free=%lu",
                    static_cast<unsigned long>(out.size()),
                    static_cast<unsigned long>(reference.size_bytes),
                    http->GetLastError(),
                    static_cast<unsigned long>(
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned long>(
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
                break;
            }
            out.append(buffer, static_cast<size_t>(received));
        }
    }
    http->Close();
    ESP_LOGI(
        TAG,
        "Owner Face reference body received=%lu expected=%lu internal=%lu "
        "largest_internal=%lu stack_free=%lu",
        static_cast<unsigned long>(out.size()),
        static_cast<unsigned long>(reference.size_bytes),
        static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
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
