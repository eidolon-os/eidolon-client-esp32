#include "hub_discovery.h"

#include "hub_txt_parser.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>

#include <cstring>

#define TAG "HubDiscovery"

namespace eidolon {

std::string HubDiscovery::NormalizeServiceType(const char* configured) {
    std::string type = configured ? configured : "_eidolon-hub";
    const std::string suffix = "._tcp";
    if (type.size() >= suffix.size() &&
        type.compare(type.size() - suffix.size(), suffix.size(), suffix) == 0) {
        type.resize(type.size() - suffix.size());
    }
    const std::string local = ".local";
    if (type.size() >= local.size() &&
        type.compare(type.size() - local.size(), local.size(), local) == 0) {
        type.resize(type.size() - local.size());
    }
    if (!type.empty() && type.front() == '_') {
        return type;
    }
    return "_eidolon-hub";
}

esp_err_t HubDiscovery::EnsureMdnsInit() {
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }
    esp_err_t err = mdns_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        initialized = true;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t HubDiscovery::QueryOnce(HubTxtRecord& best, const std::string& preferred_instance_substr, bool* found) {
    *found = false;
    best = HubTxtRecord{};

    std::string service = NormalizeServiceType(CONFIG_EIDOLON_MDNS_SERVICE_TYPE);
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr(service.c_str(), "_tcp",
                                   CONFIG_EIDOLON_MDNS_QUERY_TIMEOUT_MS, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr failed: %s", esp_err_to_name(err));
        return err;
    }

    HubTxtRecord preferred;
    bool has_preferred = false;
    HubTxtRecord fallback;
    bool has_fallback = false;

    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        std::map<std::string, std::string> entries;
        if (r->txt_count > 0 && r->txt != nullptr) {
            for (int i = 0; i < r->txt_count; ++i) {
                if (r->txt[i].key && r->txt[i].value) {
                    entries[r->txt[i].key] = r->txt[i].value;
                }
            }
        }

        HubTxtRecord parsed;
        if (HubTxtParser::Parse(entries, parsed) != ESP_OK) {
            continue;
        }

        if (r->instance_name) {
            ESP_LOGI(TAG, "Found Hub instance: %s port=%u config_url=%s",
                     r->instance_name, r->port, parsed.config_url.c_str());
        } else {
            ESP_LOGI(TAG, "Found Hub port=%u config_url=%s", r->port, parsed.config_url.c_str());
        }

        if (r->instance_name && strstr(r->instance_name, preferred_instance_substr.c_str()) != nullptr) {
            preferred = parsed;
            has_preferred = true;
        } else if (!has_fallback) {
            fallback = parsed;
            has_fallback = true;
        }
    }

    mdns_query_results_free(results);

    if (has_preferred) {
        best = preferred;
        *found = true;
        return ESP_OK;
    }
    if (has_fallback) {
        best = fallback;
        *found = true;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t HubDiscovery::Discover(HubTxtRecord& out, const std::string& preferred_instance_substr) {
    esp_err_t err = EnsureMdnsInit();
    if (err != ESP_OK) {
        return err;
    }

    bool found = false;
    for (int probe = 0; probe < CONFIG_EIDOLON_MDNS_PROBE_RETRIES; ++probe) {
        err = QueryOnce(out, preferred_instance_substr, &found);
        if (found) {
            return ESP_OK;
        }
        if (probe + 1 < CONFIG_EIDOLON_MDNS_PROBE_RETRIES) {
            ESP_LOGW(TAG, "No Hub on mDNS probe %d/%d, retrying...", probe + 1,
                     CONFIG_EIDOLON_MDNS_PROBE_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}

}  // namespace eidolon
