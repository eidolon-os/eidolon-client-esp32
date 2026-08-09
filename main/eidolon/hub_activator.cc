#include "hub_activator.h"

#include "application.h"
#include "eidolon_ui_types.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "hub_onboarding_client.h"
#include "system_info.h"

#include "sdkconfig.h"

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
        return "Waiting for approval";
    case HubConfigStatus::WaitingBinding:
        return "Waiting for Agent";
    case HubConfigStatus::Active:
        return "Device registered";
    case HubConfigStatus::Revoked:
    case HubConfigStatus::Unregistered:
        return "Device authorization required";
    }
    return "Device registration failed";
}

}  // namespace

bool HubActivator::Run() {
    auto& app = Application::GetInstance();
    const int max_retries = CONFIG_EIDOLON_MDNS_MAX_RETRIES;
    int retry_count = 0;
    int retry_delay = 10;

    HubDiscovery discovery;
    HubOnboardingClient client;
    HubConfigStore store;
    const std::string device_id = SystemInfo::GetMacAddress();

    while (retry_count < max_retries) {
        app.SetEidolonLifecycleUi(LifecyclePhase::HubDiscovering);

        HubTxtRecord txt;
        esp_err_t err = discovery.Discover(txt);
        if (err == ESP_OK) {
            app.SetEidolonLifecycleUi(LifecyclePhase::HubRegistering);
            Esp32HubConfig config;
            err = client.Run(txt, device_id, config);
            if (err == ESP_OK) {
                store.SaveTxtRecord(txt);
                store.SaveHubConfig(config, txt.descriptor_uri);

                app.SetEidolonLifecycleUi(
                    LifecyclePhase::HubRegistering,
                    config.status == HubConfigStatus::Active
                        ? "Device registered"
                        : ActivationMessageForStatus(config.status));
                return true;
            }
        }

        retry_count++;
        if (retry_count >= max_retries) {
            ESP_LOGE(TAG, "Hub activation failed after %d retries: %s", retry_count,
                     esp_err_to_name(err));
            char detail[128];
            snprintf(detail, sizeof(detail), "Hub unavailable (%s)", esp_err_to_name(err));
            app.SetEidolonLifecycleUi(LifecyclePhase::Error, detail);
            return false;
        }

        char buffer[96];
        snprintf(buffer, sizeof(buffer), "Hub retry in %ds (%d/%d)", retry_delay,
                 retry_count, max_retries);
        app.SetEidolonLifecycleUi(LifecyclePhase::HubDiscovering, buffer);

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
