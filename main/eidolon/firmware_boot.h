#pragma once

#include <esp_ota_ops.h>

namespace eidolon {
// Boot-only state. A failed init must not enter drivers that assume NVS works.
inline esp_err_t boot_storage_error = ESP_OK;

// Confirm the executable after local startup, before attempting the network.
// Persistent Owner/configuration state belongs to the existing recovery and
// admission paths, not to OTA image health. In particular, an expired directory
// is not a broken executable, and an older bootable image may erase NVS on the
// very storage fault that brought this image into recovery.
//
// Also used before board startup when NVS cannot initialize: retain this
// data-preserving recovery image across an explicit retry. Do not request an
// application rollback here. IDF still handles crashes before confirmation.
inline esp_err_t ConfirmFirmwareBoot() {
    const auto* running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;
    esp_ota_img_states_t state;
    const auto err = esp_ota_get_state_partition(running, &state);
    // Factory images and a first serial installation without an otadata record
    // have no pending verification to commit. Do not invent an OTA selection.
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    return esp_ota_mark_app_valid_cancel_rollback();
}
}  // namespace eidolon
