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
    settings.SetString("register_url", txt.register_url);
    settings.SetInt("txtvers", txt.txtvers);
    settings.SetString("hub_api", txt.api);
    settings.SetString("hub_version", txt.hub_version);
    settings.SetString("mdns_txt_json", EntriesToJson(txt.entries));
    ESP_LOGI(TAG, "Saved mDNS TXT (txtvers=%d, register_url=%s)",
             txt.txtvers, txt.register_url.c_str());
    return ESP_OK;
}

// The whole Hub config is persisted as a single JSON object under one NVS key.
// This makes a save atomic: a crash/power-loss either leaves the previous value
// intact or commits the new one whole — never a torn mix of old and new fields
// (e.g. a fresh server_url paired with a stale token).
static constexpr const char* kConfigKey = "config";

static std::string JsonStringField(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static int JsonIntField(cJSON* root, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t HubConfigStore::SaveHubConfig(const Esp32HubConfig& config,
                                        const std::string& register_url) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "register_url", register_url.c_str());
    cJSON_AddStringToObject(root, "status", HubConfigStatusToString(config.status));
    cJSON_AddStringToObject(root, "server_url", config.active.server_url.c_str());
    cJSON_AddStringToObject(root, "token", config.active.token.c_str());
    cJSON_AddStringToObject(root, "identity", config.active.identity.c_str());
    cJSON_AddStringToObject(root, "room_name", config.active.room_name.c_str());
    cJSON_AddStringToObject(root, "ctrl_url", config.control.server_url.c_str());
    cJSON_AddStringToObject(root, "ctrl_token", config.control.token.c_str());
    cJSON_AddStringToObject(root, "ctrl_id", config.control.identity.c_str());
    cJSON_AddStringToObject(root, "ctrl_room", config.control.room_name.c_str());
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

bool HubConfigStore::Load(Esp32HubConfig& config, std::string* register_url) const {
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

    std::string server_url = JsonStringField(root, "server_url");
    std::string token = JsonStringField(root, "token");
    if (server_url.empty() || token.empty()) {
        cJSON_Delete(root);
        return false;
    }

    config = Esp32HubConfig{};
    // A missing/unknown status parses to the most conservative state
    // (PendingApproval) — never silently grant voice on an absent field.
    config.status = ParseHubConfigStatus(JsonStringField(root, "status"));
    config.active.server_url = std::move(server_url);
    config.active.token = std::move(token);
    config.active.identity = JsonStringField(root, "identity");
    config.active.room_name = JsonStringField(root, "room_name");
    config.control.server_url = JsonStringField(root, "ctrl_url");
    config.control.token = JsonStringField(root, "ctrl_token");
    config.control.identity = JsonStringField(root, "ctrl_id");
    config.control.room_name = JsonStringField(root, "ctrl_room");
    config.device_fingerprint = JsonStringField(root, "fingerprint");
    config.sample_rate = JsonIntField(root, "sample_rate", 16000);
    config.channels = JsonIntField(root, "channels", 1);

    if (register_url) {
        *register_url = JsonStringField(root, "register_url");
    }
    cJSON_Delete(root);
    return true;
}

}  // namespace eidolon
