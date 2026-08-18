#include "hub_txt_parser.h"

#include <esp_log.h>

#include <cstdlib>
#include <cstring>

#define TAG "HubTxt"

namespace eidolon {

bool HubTxtParser::HasUrlScheme(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

void HubTxtParser::ApplyKnownFields(AuthorityCandidateRecord& record) {
    auto get = [&](const char* key) -> std::string {
        auto it = record.entries.find(key);
        return it != record.entries.end() ? it->second : std::string();
    };

    record.owner_domain_id = get(kTxtOwnerDomainId);
    record.owner_domain_descriptor_uri = get(kTxtOwnerDomainDescriptorUri);

    auto tv = get(kTxtVers);
    if (!tv.empty()) {
        record.txtvers = std::atoi(tv.c_str());
    }
}

esp_err_t HubTxtParser::ValidateForTxtVers(const AuthorityCandidateRecord& record) {
    if (record.txtvers <= 0) {
        ESP_LOGE(TAG, "Missing or invalid txtvers");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (record.txtvers != kSupportedTxtVers) {
        ESP_LOGE(TAG, "Unsupported txtvers=%d (supported %d)", record.txtvers, kSupportedTxtVers);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (record.owner_domain_id.empty() || record.owner_domain_id.size() > 128) {
        ESP_LOGE(TAG, "Invalid or missing owner_domain_id");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (record.owner_domain_descriptor_uri.empty() ||
        !HasUrlScheme(record.owner_domain_descriptor_uri)) {
        ESP_LOGE(TAG, "Invalid or missing owner_domain_descriptor_uri");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t HubTxtParser::Parse(const std::map<std::string, std::string>& entries,
                              AuthorityCandidateRecord& out) {
    out = AuthorityCandidateRecord{};
    out.entries = entries;
    ApplyKnownFields(out);

    for (const auto& kv : out.entries) {
        ESP_LOGD(TAG, "TXT %s=%s", kv.first.c_str(), kv.second.c_str());
    }

    return ValidateForTxtVers(out);
}

}  // namespace eidolon
