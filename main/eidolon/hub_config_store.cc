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
    settings.SetString("config_url", txt.config_url);
    settings.SetInt("txtvers", txt.txtvers);
    settings.SetString("hub_api", txt.api);
    settings.SetString("hub_version", txt.hub_version);
    settings.SetString("mdns_txt_json", EntriesToJson(txt.entries));
    ESP_LOGI(TAG, "Saved mDNS TXT (txtvers=%d, config_url=%s)", txt.txtvers, txt.config_url.c_str());
    return ESP_OK;
}

esp_err_t HubConfigStore::SaveHubConfig(const Esp32HubConfig& config, const std::string& config_url) {
    Settings settings(kNvsNamespace, true);
    settings.SetString("config_url", config_url);
    settings.SetString("config_status", HubConfigStatusToString(config.status));
    settings.SetString("server_url", config.active.server_url);
    settings.SetString("token", config.active.token);
    settings.SetString("identity", config.active.identity);
    settings.SetString("room_name", config.active.room_name);
    settings.SetString("ctrl_url", config.control.server_url);
    settings.SetString("ctrl_token", config.control.token);
    settings.SetString("ctrl_id", config.control.identity);
    settings.SetString("ctrl_room", config.control.room_name);
    settings.SetString("fingerprint", config.device_fingerprint);
    settings.SetInt("sample_rate", config.sample_rate);
    settings.SetInt("channels", config.channels);
    ESP_LOGI(TAG, "Saved Hub config identity=%s status=%s room=%s server=%s",
             config.active.identity.c_str(), HubConfigStatusToString(config.status),
             config.active.room_name.c_str(), config.active.server_url.c_str());
    return ESP_OK;
}

bool HubConfigStore::HasValidConfig() const {
    Settings settings(kNvsNamespace, false);
    return !settings.GetString("server_url").empty() && !settings.GetString("token").empty();
}

bool HubConfigStore::Load(Esp32HubConfig& config, std::string* config_url) const {
    Settings settings(kNvsNamespace, false);
    std::string server_url = settings.GetString("server_url");
    std::string token = settings.GetString("token");
    if (server_url.empty() || token.empty()) {
        return false;
    }

    config = Esp32HubConfig{};
    config.status = ParseHubConfigStatus(settings.GetString("config_status", "active"));
    config.active.server_url = std::move(server_url);
    config.active.token = std::move(token);
    config.active.identity = settings.GetString("identity");
    config.active.room_name = settings.GetString("room_name");
    config.control.server_url = settings.GetString("ctrl_url");
    config.control.token = settings.GetString("ctrl_token");
    config.control.identity = settings.GetString("ctrl_id");
    config.control.room_name = settings.GetString("ctrl_room");
    config.device_fingerprint = settings.GetString("fingerprint");
    config.sample_rate = settings.GetInt("sample_rate", 16000);
    config.channels = settings.GetInt("channels", 1);

    if (config_url) {
        *config_url = settings.GetString("config_url");
    }
    return true;
}

}  // namespace eidolon
