#include "hub_txt_parser.h"

#include <esp_log.h>

#include <cstdlib>
#include <cstring>

#define TAG "HubTxt"

namespace eidolon {

bool HubTxtParser::HasUrlScheme(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

void HubTxtParser::ApplyKnownFields(HubTxtRecord& record) {
    auto get = [&](const char* key) -> std::string {
        auto it = record.entries.find(key);
        return it != record.entries.end() ? it->second : std::string();
    };

    record.api = get(kTxtApi);
    record.config_url = get(kTxtConfigUrl);
    record.hub_version = get(kTxtVersion);

    auto tv = get(kTxtVers);
    if (!tv.empty()) {
        record.txtvers = std::atoi(tv.c_str());
    }
}

esp_err_t HubTxtParser::ValidateForTxtVers(const HubTxtRecord& record) {
    if (record.txtvers <= 0) {
        ESP_LOGE(TAG, "Missing or invalid txtvers");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (record.txtvers > kSupportedMaxTxtVers) {
        ESP_LOGE(TAG, "Unsupported txtvers=%d (max %d)", record.txtvers, kSupportedMaxTxtVers);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (record.txtvers != kSupportedTxtVers) {
        ESP_LOGE(TAG, "Unsupported txtvers=%d (supported %d)", record.txtvers, kSupportedTxtVers);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (record.api != kExpectedApi) {
        ESP_LOGE(TAG, "Unsupported api=%s (expected %s)", record.api.c_str(), kExpectedApi);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (record.config_url.empty() || !HasUrlScheme(record.config_url)) {
        ESP_LOGE(TAG, "Invalid or missing config_url");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t HubTxtParser::Parse(const std::map<std::string, std::string>& entries, HubTxtRecord& out) {
    out = HubTxtRecord{};
    out.entries = entries;
    ApplyKnownFields(out);

    for (const auto& kv : out.entries) {
        ESP_LOGD(TAG, "TXT %s=%s", kv.first.c_str(), kv.second.c_str());
    }

    return ValidateForTxtVers(out);
}

}  // namespace eidolon
