#pragma once

#include <esp_ota_ops.h>
#include "commissioning_transaction.h"
#include "authority_locator.h"
#include "esp_idf_commissioning_credential_store.h"
#include "hub_trust_store.h"

namespace eidolon {
// Boot-only state. A failed init must not enter drivers that assume NVS works.
inline esp_err_t boot_storage_error = ESP_OK;

inline bool FirmwareNeedsVerification() {
    const auto* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

inline esp_err_t RecoverFirmwareBoot() {
    if (!FirmwareNeedsVerification()) return ESP_OK;
    if (esp_ota_check_rollback_is_possible()) {
        // On success IDF reboots into the previous app; it never rewinds NVS.
        const auto err = esp_ota_mark_app_invalid_rollback_and_reboot();
        if (err == ESP_OK) return ESP_OK;
    }
    // No usable fallback (e.g. initial installation): keep this recovery-capable
    // image bootable. Leaving PENDING_VERIFY here can abort the only app on reset.
    return esp_ota_mark_app_valid_cancel_rollback();
}

inline esp_err_t ConfirmFirmwareBoot() {
    if (!FirmwareNeedsVerification()) return ESP_OK;
    OwnerTrustBundle trust;
    std::string key;
    const bool recovered = RecoverPendingCommissioningTransaction();
    const auto trust_state = OwnerTrustStore().ReadActive(trust);
    const auto identity = EspIdfCommissioningCredentialStore::LoadPrivateKey(key);
    const bool readable = boot_storage_error == ESP_OK && recovered &&
        trust_state != OwnerTrustLoadResult::Unavailable &&
        identity != CommissioningIdentityLoad::Unavailable &&
        (trust_state != OwnerTrustLoadResult::Loaded || identity == CommissioningIdentityLoad::Loaded);
    const bool directory_valid = trust_state != OwnerTrustLoadResult::Loaded ||
        DeviceAuthorityLocator::GetInstance().ReloadCommissionedDirectory() == ESP_OK;
    if (!readable || !directory_valid) {
        RecoverFirmwareBoot();
        return ESP_ERR_INVALID_STATE;
    }
    // Board/UI startup and local recovery are enough; Wi-Fi and the Hub may be
    // offline. They must not keep a working firmware in PENDING_VERIFY.
    return esp_ota_mark_app_valid_cancel_rollback();
}
}  // namespace eidolon
