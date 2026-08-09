#include "hub_config_store.h"

#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#define TAG "HubConfigStore"

namespace eidolon {

static std::string EntriesToJson(const std::map<std::string, std::string>& entries) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return "{}";
    }
    for (const auto& kv : entries) {
        cJSON_AddStringToObject(root, kv.first.c_str(), kv.second.c_str());
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return json;
}

esp_err_t HubConfigStore::SaveTxtRecord(const HubTxtRecord& txt) {
    Settings settings(kNvsNamespace, true);
    settings.SetString("descriptor_uri", txt.descriptor_uri);
    settings.SetString("enrollment_uri", txt.enrollment_uri);
    settings.SetInt("txtvers", txt.txtvers);
    settings.SetString("mdns_txt_json", EntriesToJson(txt.entries));
    ESP_LOGI(TAG, "Saved mDNS TXT (txtvers=%d, descriptor_uri=%s)",
             txt.txtvers, txt.descriptor_uri.c_str());
    return ESP_OK;
}

// The whole Hub config is persisted as a single JSON object under one NVS key.
// This makes a save atomic: a crash/power-loss either leaves the previous value
// intact or commits the new one whole — never a torn mix of old and new fields
// (e.g. a fresh server_url paired with a stale token).
static constexpr const char* kConfigKey = "config";
static constexpr const char* kOnboardingKey = "onboarding";
static constexpr int kConfigSchemaVersion = 2;

static std::string JsonStringField(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static int JsonIntField(cJSON* root, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t HubConfigStore::SaveHubConfig(const Esp32HubConfig& config,
                                        const std::string& descriptor_uri) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", kConfigSchemaVersion);
    cJSON_AddStringToObject(root, "descriptor_uri", descriptor_uri.c_str());
    cJSON_AddStringToObject(root, "status", HubConfigStatusToString(config.status));
    cJSON_AddStringToObject(root, "server_url", config.active.server_url.c_str());
    cJSON_AddStringToObject(root, "token", config.active.token.c_str());
    cJSON_AddStringToObject(root, "identity", config.active.identity.c_str());
    cJSON_AddStringToObject(root, "room_name", config.active.room_name.c_str());
    cJSON_AddStringToObject(root, "ctrl_url", config.control.server_url.c_str());
    cJSON_AddStringToObject(root, "ctrl_token", config.control.token.c_str());
    cJSON_AddStringToObject(root, "ctrl_id", config.control.identity.c_str());
    cJSON_AddStringToObject(root, "ctrl_room", config.control.room_name.c_str());
    cJSON_AddStringToObject(root, "registration_id", config.registration_id.c_str());
    cJSON_AddStringToObject(root, "fingerprint", config.device_fingerprint.c_str());
    cJSON_AddNumberToObject(root, "sample_rate", config.sample_rate);
    cJSON_AddNumberToObject(root, "channels", config.channels);

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    {
        Settings settings(kNvsNamespace, true);
        settings.SetString(kConfigKey, printed);
    }
    cJSON_free(printed);

    ESP_LOGI(TAG, "Saved Hub config identity=%s status=%s room=%s server=%s",
             config.active.identity.c_str(), HubConfigStatusToString(config.status),
             config.active.room_name.c_str(), config.active.server_url.c_str());
    return ESP_OK;
}

bool HubConfigStore::HasValidConfig() const {
    Esp32HubConfig config;
    return Load(config, nullptr);
}

bool HubConfigStore::Load(Esp32HubConfig& config, std::string* descriptor_uri) const {
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
    config.active.server_url = JsonStringField(root, "server_url");
    config.active.token = JsonStringField(root, "token");
    config.active.identity = JsonStringField(root, "identity");
    config.active.room_name = JsonStringField(root, "room_name");
    config.control.server_url = JsonStringField(root, "ctrl_url");
    config.control.token = JsonStringField(root, "ctrl_token");
    config.control.identity = JsonStringField(root, "ctrl_id");
    config.control.room_name = JsonStringField(root, "ctrl_room");
    config.registration_id = JsonStringField(root, "registration_id");
    config.device_fingerprint = JsonStringField(root, "fingerprint");
    config.sample_rate = JsonIntField(root, "sample_rate", 16000);
    config.channels = JsonIntField(root, "channels", 1);

    const bool valid =
        config.status != HubConfigStatus::Active || config.active.usable();
    if (descriptor_uri) {
        *descriptor_uri = JsonStringField(root, "descriptor_uri");
    }
    cJSON_Delete(root);
    return valid && (!descriptor_uri || !descriptor_uri->empty());
}

esp_err_t HubConfigStore::SaveOnboardingState(const HubOnboardingState& state) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "hub_id", state.hub_id.c_str());
    cJSON_AddStringToObject(root, "descriptor_uri", state.descriptor_uri.c_str());
    cJSON_AddStringToObject(root, "enrollment_uri", state.enrollment_uri.c_str());
    cJSON_AddStringToObject(root, "device_id", state.device_id.c_str());
    cJSON_AddStringToObject(root, "request_id", state.request_id.c_str());
    cJSON_AddStringToObject(root, "retrieval_token", state.retrieval_token.c_str());
    cJSON_AddStringToObject(root, "pairing_secret", state.pairing_secret.c_str());
    cJSON_AddStringToObject(root, "pairing_commitment", state.pairing_commitment.c_str());
    cJSON_AddStringToObject(root, "enrollment_id", state.enrollment_id.c_str());
    cJSON_AddStringToObject(root, "pairing_claim_uri", state.pairing_claim_uri.c_str());
    cJSON_AddStringToObject(root, "lifecycle_state", state.lifecycle_state.c_str());
    cJSON_AddNumberToObject(root, "retrieval_expires_at_ms",
                           static_cast<double>(state.retrieval_expires_at_ms));
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    Settings settings(kNvsNamespace, true);
    settings.SetString(kOnboardingKey, printed);
    cJSON_free(printed);
    return ESP_OK;
}

bool HubConfigStore::LoadOnboardingState(HubOnboardingState& state) const {
    Settings settings(kNvsNamespace, false);
    const std::string blob = settings.GetString(kOnboardingKey);
    cJSON* root = blob.empty() ? nullptr : cJSON_Parse(blob.c_str());
    if (!root || JsonIntField(root, "schema_version", 0) != 1) {
        cJSON_Delete(root);
        return false;
    }
    state = HubOnboardingState{};
    state.hub_id = JsonStringField(root, "hub_id");
    state.descriptor_uri = JsonStringField(root, "descriptor_uri");
    state.enrollment_uri = JsonStringField(root, "enrollment_uri");
    state.device_id = JsonStringField(root, "device_id");
    state.request_id = JsonStringField(root, "request_id");
    state.retrieval_token = JsonStringField(root, "retrieval_token");
    state.pairing_secret = JsonStringField(root, "pairing_secret");
    state.pairing_commitment = JsonStringField(root, "pairing_commitment");
    state.enrollment_id = JsonStringField(root, "enrollment_id");
    state.pairing_claim_uri = JsonStringField(root, "pairing_claim_uri");
    state.lifecycle_state = JsonStringField(root, "lifecycle_state");
    const cJSON* expires = cJSON_GetObjectItem(root, "retrieval_expires_at_ms");
    if (cJSON_IsNumber(expires) && expires->valuedouble >= 0) {
        state.retrieval_expires_at_ms =
            static_cast<int64_t>(expires->valuedouble);
    }
    cJSON_Delete(root);
    return state.resumable();
}

void HubConfigStore::ClearOnboardingState() {
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kOnboardingKey);
}

}  // namespace eidolon
