#include <cassert>
#include <iostream>
#include "eidolon/firmware_boot.h"

// Intentionally link no trust, identity, clock, transaction or network adapter.
// OTA must not read/migrate Owner data or judge a cached route's lifetime.
// Those operations have their own recovery/admission owners.
namespace {
bool running_exists = true;
esp_ota_img_states_t state = ESP_OTA_IMG_PENDING_VERIFY;
esp_err_t read_result = ESP_OK, write_result = ESP_OK;
int confirmations = 0, state_reads = 0;
void Reset() {
    running_exists = true;
    state = ESP_OTA_IMG_PENDING_VERIFY;
    read_result = write_result = ESP_OK;
    confirmations = state_reads = 0;
    eidolon::boot_storage_error = ESP_OK;
}
}
const esp_partition_t* esp_ota_get_running_partition() {
    static esp_partition_t partition{};
    return running_exists ? &partition : nullptr;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* out) {
    ++state_reads;
    if (read_result == ESP_OK) *out = state;
    return read_result;
}
bool esp_ota_check_rollback_is_possible() {
    assert(false && "bootability cannot prove a previous image preserves NVS");
    return true;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    assert(false && "the previous image may erase NVS on the same storage fault");
    return ESP_FAIL;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++confirmations;
    if (write_result == ESP_OK) state = ESP_OTA_IMG_VALID;
    return write_result;
}
int main() {
    using eidolon::ConfirmFirmwareBoot;
    Reset();
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmations == 1);
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmations == 1); // idempotent

    // A storage fault retains this recovery executable. It must not downgrade
    // or clear the fault that keeps Application/WifiBoard in recovery.
    Reset(); eidolon::boot_storage_error = ESP_FAIL;
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmations == 1);
    assert(eidolon::boot_storage_error == ESP_FAIL);

    for (const auto error : {ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED}) {
        Reset(); read_result = error;
        assert(ConfirmFirmwareBoot() == ESP_OK && confirmations == 0);
    }
    Reset(); state = ESP_OTA_IMG_VALID;
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmations == 0);
    Reset(); running_exists = false;
    assert(ConfirmFirmwareBoot() == ESP_ERR_INVALID_STATE && state_reads == 0);
    Reset(); read_result = ESP_FAIL;
    assert(ConfirmFirmwareBoot() == ESP_FAIL && confirmations == 0);
    Reset(); write_result = ESP_FAIL;
    assert(ConfirmFirmwareBoot() == ESP_FAIL && state == ESP_OTA_IMG_PENDING_VERIFY);
    // A failed otadata write is visible, not reported as a successful recovery.
    write_result = ESP_OK;
    assert(ConfirmFirmwareBoot() == ESP_OK && state == ESP_OTA_IMG_VALID);
    std::cout << "firmware offline confirmation, storage-safe recovery and OTA errors: PASS\n";
}
