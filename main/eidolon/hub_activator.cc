#include "hub_activator.h"

#include "application.h"
#include "commissioning_runtime.h"
#include "device_boot_recovery.h"
#include "device_identity.h"
#include "eidolon_ui_types.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "hub_onboarding_client.h"

#include "sdkconfig.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#define TAG "HubActivator"

namespace eidolon {

namespace {

void ProjectHubConfig(Application& app, HubConfigStatus status)
{
    switch (status) {
    case HubConfigStatus::PendingApproval:
        app.SetEidolonEnrollmentUi(EnrollmentPhase::PendingReview);
        app.SetEidolonServiceUi(ServicePhase::Unavailable);
        break;
    case HubConfigStatus::WaitingBinding:
        app.SetEidolonEnrollmentUi(EnrollmentPhase::ClaimActive);
        app.SetEidolonServiceUi(ServicePhase::Preparing);
        break;
    case HubConfigStatus::Active:
        app.SetEidolonEnrollmentUi(EnrollmentPhase::ClaimActive);
        app.SetEidolonServiceUi(ServicePhase::Ready);
        break;
    case HubConfigStatus::Revoked:
        app.SetEidolonEnrollmentUi(EnrollmentPhase::Revoked);
        app.SetEidolonServiceUi(ServicePhase::Unavailable);
        break;
    }
}

}  // namespace

bool HubActivator::Run() {
    auto& app = Application::GetInstance();
    int retry_delay = 10;

    const DeviceEraseCoreOutcome removal_recovery =
        DeviceBootRecovery::ResumePendingRemoval();
    if (!DeviceBootRecovery::AllowsClaimOrRuntime(removal_recovery)) {
        ESP_LOGE(TAG,
                 "RemovalJournal blocks Claim/runtime until recovery is terminal result=%d",
                 static_cast<int>(removal_recovery.result));
        app.SetEidolonRuntimeUi(
            RuntimePhase::RecoveryRequired,
            "Device removal recovery required");
        return false;
    }

    HubDiscovery discovery;
    HubOnboardingClient client;
    HubConfigStore store;
    auto& identity = DeviceIdentity::GetInstance();
    if (identity.EnsureKeypair() != ESP_OK) {
        ESP_LOGE(TAG, "Operational identity is unavailable; refusing activation");
        return false;
    }
    const std::string device_id = identity.DeviceInstanceId();

    for (;;) {
        // Admission consumes a confirmed Station route; it never competes with
        // a commissioning generation for the radio or projects stale Hub state
        // over the commissioning actor's UI. The network-connected handoff
        // starts a new ActivationTask after StationRouteReady.
        if (CommissioningRuntime::GetInstance().IsInProgress()) {
            ESP_LOGI(TAG, "Commissioning owns the RadioLease; suspending Hub activation");
            return false;
        }
        app.SetEidolonServiceUi(ServicePhase::DiscoveringAuthority);

        AuthorityCandidateRecord txt;
        esp_err_t err = discovery.Discover(txt);
        if (err == ESP_OK) {
            app.SetEidolonServiceUi(ServicePhase::Registering);
            Esp32HubConfig config;
            err = client.Run(txt, device_id, config);
            if (err == ESP_OK) {
                if (CommissioningRuntime::GetInstance().IsInProgress()) {
                    ESP_LOGI(TAG, "Commissioning began during Hub handoff; deferring activation");
                    return false;
                }
                if (store.SaveHubConfig(config) != ESP_OK) {
                    // The Host already admitted this device and the credential is
                    // in hand; only the cache of it failed. Refusing to continue
                    // turned a full NVS partition into a device that could not be
                    // used at all, when what it actually costs is re-activating on
                    // the next boot instead of resuming offline.
                    ESP_LOGW(TAG, "Hub activation could not be cached; using it for this session");
                }

                ProjectHubConfig(app, config.status);
                return true;
            }
        }

        if (CommissioningRuntime::GetInstance().IsInProgress()) {
            ESP_LOGI(TAG, "Commissioning owns the RadioLease; suspending Hub activation");
            return false;
        }

        // Keep asking. A Host that is switched off, a network still coming back,
        // a device nobody has set up yet — none of those are permanent, and
        // giving up after ten tries turned every one of them into a device that
        // needed a power cycle to try again. What ends this loop is success, or
        // the device being put to use another way.
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "Looking for the Hub again in %ds", retry_delay);
        app.SetEidolonServiceUi(ServicePhase::DiscoveringAuthority, buffer);

        ESP_LOGW(TAG, "Hub activation failed (%s), retry in %ds", esp_err_to_name(err),
                 retry_delay);

        for (int i = 0; i < retry_delay; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (CommissioningRuntime::GetInstance().IsInProgress()) {
                ESP_LOGI(TAG, "Commissioning owns the RadioLease; suspending Hub activation");
                return false;
            }
            if (app.GetDeviceState() == kDeviceStateIdle) {
                return false;
            }
        }
        retry_delay = retry_delay * 2;
        if (retry_delay > 120) {
            retry_delay = 120;
        }
    }
}

}  // namespace eidolon
