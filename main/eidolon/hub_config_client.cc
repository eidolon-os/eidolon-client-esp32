#include "hub_config_client.h"

#include "board.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>

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

}  // namespace

esp_err_t HubConfigClient::Fetch(const std::string& config_url, const std::string& device_id,
                                 Esp32HubConfig& out) {
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

    http->SetHeader("X-Device-ID", device_id.c_str());
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());

    std::string request_url = config_url;
    const char* agent_param = "agent_mode=streaming";
    if (request_url.find('?') != std::string::npos) {
        request_url += "&";
        request_url += agent_param;
    } else {
        request_url += "?";
        request_url += agent_param;
    }

    if (!http->Open("GET", request_url)) {
        ESP_LOGE(TAG, "HTTP open failed for %s", request_url.c_str());
        return ESP_FAIL;
    }

    int status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, config_url.c_str());
        return status == 422 ? ESP_ERR_INVALID_ARG : ESP_FAIL;
    }

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

    out.server_url = server_url->valuestring;
    out.token = token->valuestring;
    out.identity = identity->valuestring;
    out.room_name = room_name->valuestring;

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

    ParseOptionalFirmware(root, &has_pending_firmware_, &pending_firmware_force_,
                         &pending_firmware_version_, &pending_firmware_url_);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Fetched Hub config for %s", out.identity.c_str());
    return ESP_OK;
}

}  // namespace eidolon
