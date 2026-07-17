#include "hub_activator.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display.h"
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "ota.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <cJSON.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#define TAG "HubActivator"

namespace eidolon {

namespace {

const char* ActivationMessageForStatus(HubConfigStatus status)
{
    switch (status) {
    case HubConfigStatus::PendingApproval:
        return Lang::Strings::EIDOLON_WAITING_APPROVAL;
    case HubConfigStatus::WaitingBinding:
        return Lang::Strings::EIDOLON_WAITING_BINDING;
    case HubConfigStatus::Active:
        return Lang::Strings::EIDOLON_READY;
    case HubConfigStatus::Revoked:
    case HubConfigStatus::Unregistered:
        return Lang::Strings::ERROR;
    }
    return Lang::Strings::ERROR;
}

}  // namespace

bool HubActivator::Run(Display* display) {
    auto& app = Application::GetInstance();
    const int max_retries = CONFIG_EIDOLON_MDNS_MAX_RETRIES;
    int retry_count = 0;
    int retry_delay = 10;

    HubDiscovery discovery;
    HubConfigClient client;
    HubConfigStore store;
    const std::string device_id = SystemInfo::GetMacAddress();

    while (retry_count < max_retries) {
        if (display) {
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
        }

        HubTxtRecord txt;
        esp_err_t err = discovery.Discover(txt);
        if (err == ESP_OK) {
            Esp32HubConfig config;
            err = client.RegisterDevice(txt.register_url, device_id, config);
            if (err == ESP_OK) {
                store.SaveTxtRecord(txt);
                store.SaveHubConfig(config, txt.register_url);

                if (client.HasPendingFirmware()) {
                    cJSON* firmware = cJSON_CreateObject();
                    cJSON_AddStringToObject(firmware, "version",
                                            client.PendingFirmwareVersion().c_str());
                    cJSON_AddStringToObject(firmware, "url", client.PendingFirmwareUrl().c_str());
                    if (client.PendingFirmwareForce()) {
                        cJSON_AddNumberToObject(firmware, "force", 1);
                    }
                    Ota ota;
                    if (ota.ApplyFirmwareSection(firmware)) {
                        ESP_LOGI(TAG, "Firmware upgrade pending (phase 2): %s",
                                 ota.GetFirmwareUrl().c_str());
                    }
                    cJSON_Delete(firmware);
                }

                if (display) {
                    display->SetChatMessage("system", ActivationMessageForStatus(config.status));
                }
                return true;
            }
        }

        retry_count++;
        if (retry_count >= max_retries) {
            ESP_LOGE(TAG, "Hub activation failed after %d retries: %s", retry_count,
                     esp_err_to_name(err));
            char detail[128];
            snprintf(detail, sizeof(detail), "Hub err=%d", (int)err);
            app.Alert(Lang::Strings::ERROR, detail, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);
            return false;
        }

        char buffer[256];
        snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                 esp_err_to_name(err));
        app.Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

        ESP_LOGW(TAG, "Hub activation failed (%s), retry in %ds (%d/%d)",
                 esp_err_to_name(err), retry_delay, retry_count, max_retries);

        for (int i = 0; i < retry_delay; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (app.GetDeviceState() == kDeviceStateIdle) {
                return false;
            }
        }
        retry_delay = retry_delay * 2;
        if (retry_delay > 120) {
            retry_delay = 120;
        }
    }

    return false;
}

}  // namespace eidolon
