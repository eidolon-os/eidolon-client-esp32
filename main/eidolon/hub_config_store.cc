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
    settings.SetString("server_url", config.server_url);
    settings.SetString("token", config.token);
    settings.SetString("identity", config.identity);
    settings.SetString("room_name", config.room_name);
    settings.SetInt("sample_rate", config.sample_rate);
    settings.SetInt("channels", config.channels);
    ESP_LOGI(TAG, "Saved Hub config identity=%s room=%s server=%s",
             config.identity.c_str(), config.room_name.c_str(), config.server_url.c_str());
    return ESP_OK;
}

bool HubConfigStore::HasValidConfig() const {
    Settings settings(kNvsNamespace, false);
    return !settings.GetString("server_url").empty() && !settings.GetString("token").empty();
}

}  // namespace eidolon
