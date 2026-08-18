#include "hub_config_store.h"

#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#define TAG "HubConfigStore"

namespace eidolon {

// The whole Hub config is persisted as a single JSON object under one NVS key.
// This makes a save atomic: a crash/power-loss either leaves the previous value
// intact or commits the new one whole — never a torn mix of old and new fields
// (e.g. a fresh server_url paired with a stale token).
static constexpr const char* kConfigKey = "config";
static constexpr const char* kOnboardingKey = "onboarding";
// Version 4 holds one channel. Version 3 held a voice room and a control room,
// and a device reading those back would connect to a pair that no longer exists
// — the Provider issues one. Refusing the old record sends the device to
// re-provision, which is the only way to be handed the new one.
static constexpr int kConfigSchemaVersion = 5;
// Version 2 is the screen-independent manual-admission state. Version 1 held
// abandoned screen-bound claim material and must start a fresh Enrollment intent.
static constexpr int kOnboardingSchemaVersion = 3;

static std::string JsonStringField(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static int JsonIntField(cJSON* root, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t HubConfigStore::SaveHubConfig(const Esp32HubConfig& config) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", kConfigSchemaVersion);
    cJSON_AddStringToObject(root, "status", HubConfigStatusToString(config.status));
    cJSON_AddStringToObject(root, "server_url", config.session.server_url.c_str());
    cJSON_AddStringToObject(root, "token", config.session.token.c_str());
    cJSON_AddStringToObject(root, "identity", config.session.identity.c_str());
    cJSON_AddStringToObject(root, "room_name", config.session.room_name.c_str());
    cJSON_AddNumberToObject(root, "expires_at_ms",
                           static_cast<double>(config.expires_at_ms));
    cJSON_AddNumberToObject(root, "sample_rate", config.sample_rate);
    cJSON_AddNumberToObject(root, "channels", config.channels);

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    Settings settings(kNvsNamespace, true);
    const esp_err_t write_err = settings.SetString(kConfigKey, printed);
    cJSON_free(printed);
    if (write_err != ESP_OK) {
        return write_err;
    }
    const esp_err_t commit_err = settings.Commit();
    if (commit_err != ESP_OK) {
        return commit_err;
    }

    ESP_LOGI(TAG, "Saved Hub config identity=%s status=%s room=%s server=%s",
             config.session.identity.c_str(), HubConfigStatusToString(config.status),
             config.session.room_name.c_str(), config.session.server_url.c_str());
    return ESP_OK;
}

bool HubConfigStore::HasValidConfig() const {
    Esp32HubConfig config;
    return Load(config);
}

bool HubConfigStore::Load(Esp32HubConfig& config) const {
    Settings settings(kNvsNamespace, false);
    std::string blob = settings.GetString(kConfigKey);
    if (blob.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(blob.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Stored Hub config is not valid JSON; treating as no config");
        return false;
    }

    if (JsonIntField(root, "schema_version", 0) != kConfigSchemaVersion) {
        cJSON_Delete(root);
        return false;
    }

    config = Esp32HubConfig{};
    // A missing/unknown status parses to the most conservative state
    // (PendingApproval) — never silently grant voice on an absent field.
    config.status = ParseHubConfigStatus(JsonStringField(root, "status"));
    config.session.server_url = JsonStringField(root, "server_url");
    config.session.token = JsonStringField(root, "token");
    config.session.identity = JsonStringField(root, "identity");
    config.session.room_name = JsonStringField(root, "room_name");
    const cJSON* expires = cJSON_GetObjectItem(root, "expires_at_ms");
    if (cJSON_IsNumber(expires) && expires->valuedouble >= 0) {
        config.expires_at_ms = static_cast<int64_t>(expires->valuedouble);
    }
    config.sample_rate = JsonIntField(root, "sample_rate", 16000);
    config.channels = JsonIntField(root, "channels", 1);

    const bool valid =
        config.status != HubConfigStatus::Active || config.session.usable();
    cJSON_Delete(root);
    return valid;
}

esp_err_t HubConfigStore::SaveOnboardingState(const HubOnboardingState& state) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", kOnboardingSchemaVersion);
    cJSON_AddStringToObject(root, "owner_domain_id", state.owner_domain_id.c_str());
    cJSON_AddNumberToObject(root, "directory_revision",
                           static_cast<double>(state.directory_revision));
    cJSON_AddStringToObject(root, "device_id", state.device_id.c_str());
    cJSON_AddStringToObject(root, "request_id", state.request_id.c_str());
    cJSON_AddStringToObject(root, "retrieval_token", state.retrieval_token.c_str());
    cJSON_AddStringToObject(root, "enrollment_id", state.enrollment_id.c_str());
    cJSON_AddStringToObject(root, "lifecycle_state", state.lifecycle_state.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    Settings settings(kNvsNamespace, true);
    const esp_err_t write_err = settings.SetString(kOnboardingKey, printed);
    cJSON_free(printed);
    return write_err == ESP_OK ? settings.Commit() : write_err;
}

bool HubConfigStore::LoadOnboardingState(HubOnboardingState& state) const {
    Settings settings(kNvsNamespace, false);
    const std::string blob = settings.GetString(kOnboardingKey);
    cJSON* root = blob.empty() ? nullptr : cJSON_Parse(blob.c_str());
    if (!root || JsonIntField(root, "schema_version", 0) !=
                     kOnboardingSchemaVersion) {
        cJSON_Delete(root);
        return false;
    }
    state = HubOnboardingState{};
    state.owner_domain_id = JsonStringField(root, "owner_domain_id");
    state.directory_revision = static_cast<uint64_t>(
        JsonIntField(root, "directory_revision", 0));
    state.device_id = JsonStringField(root, "device_id");
    state.request_id = JsonStringField(root, "request_id");
    state.retrieval_token = JsonStringField(root, "retrieval_token");
    state.enrollment_id = JsonStringField(root, "enrollment_id");
    state.lifecycle_state = JsonStringField(root, "lifecycle_state");
    cJSON_Delete(root);
    return !state.owner_domain_id.empty() && state.directory_revision > 0 &&
           !state.device_id.empty() &&
           state.has_local_intent();
}

void HubConfigStore::ClearOnboardingState() {
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kOnboardingKey);
}

}  // namespace eidolon
