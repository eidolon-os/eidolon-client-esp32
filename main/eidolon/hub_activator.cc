#include "hub_activator.h"

#include "application.h"
#include "commissioning_runtime.h"
#include "device_boot_recovery.h"
#include "device_identity.h"
#include "eidolon_ui_types.h"
#include "hub_activation_retry_core.h"
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

ActivationAttemptOutcome ClassifyAttempt(esp_err_t err, const Esp32HubConfig& config)
{
    if (err == ESP_OK) {
        return ActivationAttemptOutcome::Admitted;
    }
    // Only the lifecycle channel says a Claim is terminal. ESP_ERR_NOT_ALLOWED
    // alone does not: the Authority answering 401/403 has rejected a request,
    // not decided anything about this device, and showing its Owner "you were
    // removed" for a rejected request points the person at the wrong problem.
    if (err == ESP_ERR_NOT_ALLOWED && config.status == HubConfigStatus::Revoked) {
        return ActivationAttemptOutcome::ClaimTerminal;
    }
    return ActivationAttemptOutcome::Retryable;
}

}  // namespace

bool HubActivator::Run() {
    auto& app = Application::GetInstance();
    HubActivationRetryCore retry;

    const DeviceEraseCoreOutcome removal_recovery =
        DeviceBootRecovery::ResumePendingRemoval();
    if (!DeviceBootRecovery::AllowsClaimOrRuntime(removal_recovery)) {
        ESP_LOGE(TAG,
                 "RemovalJournal blocks Claim/runtime until recovery is terminal result=%d",
                 static_cast<int>(removal_recovery.result));
        // Name the gesture, because this is the screen the person is actually
        // looking at. The Claim-terminal path below says the same sentence, but
        // it is not reached on a device the journal already stopped here — and
        // "recovery required" on its own tells somebody standing at the device
        // that something is wrong without telling them the one thing that
        // fixes it. Setup no longer opens by itself, by design, so the button
        // is not a hint; it is the only way back.
        app.SetEidolonRuntimeUi(
            RuntimePhase::RecoveryRequired,
            "Removed from this Owner. Press and hold the button to claim it again");
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
        if (retry.Evaluate(CommissioningRuntime::GetInstance().IsInProgress()) !=
            ActivationStandDown::KeepAsking) {
            ESP_LOGI(TAG, "Commissioning owns the RadioLease; suspending Hub activation");
            return false;
        }
        app.SetEidolonServiceUi(ServicePhase::DiscoveringAuthority);

        Esp32HubConfig config;
        AuthorityCandidateRecord txt;
        esp_err_t err = discovery.Discover(txt);
        if (err == ESP_OK) {
            app.SetEidolonServiceUi(ServicePhase::Registering);
            err = client.Run(txt, device_id, config);
        }

        // A commissioning generation may have taken the radio while the attempt
        // was in flight; its UI and its RadioLease outrank this result.
        if (retry.Evaluate(CommissioningRuntime::GetInstance().IsInProgress()) !=
            ActivationStandDown::KeepAsking) {
            ESP_LOGI(TAG, "Commissioning began during Hub handoff; deferring activation");
            return false;
        }

        switch (retry.OnAttempt(ClassifyAttempt(err, config))) {
        case ActivationStandDown::Admitted:
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

        case ActivationStandDown::ClaimTerminal:
            // Removal and revocation are deliberately terminal: the firmware
            // does not silently resurrect a revoked lifecycle. Asking again
            // cannot change the answer, so say what does — and say it as the
            // gesture, not as "open setup". The device cannot open setup for
            // itself here, and it must not: WifiBoard::StartWifiConfigMode
            // refuses while the RemovalJournal stands. A person holding the
            // button is the only thing that changes this answer.
            ESP_LOGW(TAG, "Claim is terminal (%s); physical presence is required to rejoin",
                     esp_err_to_name(err));
            app.SetEidolonEnrollmentUi(EnrollmentPhase::Revoked);
            app.SetEidolonServiceUi(ServicePhase::Unavailable);
            app.SetEidolonRuntimeUi(
                RuntimePhase::RecoveryRequired,
                "Removed from this Owner. Press and hold the button to claim it again");
            return false;

        case ActivationStandDown::CommissioningOwnsRadio:
            return false;

        case ActivationStandDown::KeepAsking:
            break;
        }

        // Keep asking. A Host that is switched off, a network still coming back,
        // a device nobody has set up yet — none of those are permanent, and
        // giving up after ten tries turned every one of them into a device that
        // needed a power cycle to try again. What ends this loop is success, a
        // commissioning generation taking the radio, or a terminal Claim.
        const int retry_delay = retry.delay_seconds();
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "Looking for the Hub again in %ds", retry_delay);
        app.SetEidolonServiceUi(ServicePhase::DiscoveringAuthority, buffer);

        ESP_LOGW(TAG, "Hub activation failed (%s), attempt %d, retry in %ds",
                 esp_err_to_name(err), retry.retryable_attempts(), retry_delay);

        for (int i = 0; i < retry_delay; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (retry.Evaluate(CommissioningRuntime::GetInstance().IsInProgress()) !=
                ActivationStandDown::KeepAsking) {
                ESP_LOGI(TAG, "Commissioning owns the RadioLease; suspending Hub activation");
                return false;
            }
        }
    }
}

}  // namespace eidolon
