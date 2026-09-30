#include "eidolon/firmware_boot.h"
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <cassert>
#include <cstring>

namespace {
constexpr char kIdentity[] = "persistent-owner-identity-must-survive";
void Next(nvs_handle_t nvs, uint8_t stage, const esp_partition_t* candidate = nullptr) {
    ESP_ERROR_CHECK(nvs_set_u8(nvs, "stage", stage));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    if (candidate) ESP_ERROR_CHECK(esp_ota_set_boot_partition(candidate));
    esp_restart();
}
esp_ota_img_states_t State(const esp_partition_t* partition) {
    esp_ota_img_states_t state;
    ESP_ERROR_CHECK(esp_ota_get_state_partition(partition, &state));
    return state;
}
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("boot_contract", NVS_READWRITE, &nvs));
    uint8_t stage = 0;
    auto result = nvs_get_u8(nvs, "stage", &stage);
    assert(result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND);
    const auto* running = esp_ota_get_running_partition();
    const auto* other = esp_ota_get_next_update_partition(running);
    assert(running && other);
    const bool slot0 = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
    ESP_LOGI("BOOT_TEST", "stage=%u slot=%s", stage, running->label);

    if (stage == 0) {
        assert(slot0);
        ESP_ERROR_CHECK(nvs_set_str(nvs, "identity", kIdentity));
        // Establish a valid rollback image with the official OTA APIs.
        ESP_ERROR_CHECK(esp_ota_set_boot_partition(running));
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        Next(nvs, 1, other);
    }
    char identity[sizeof(kIdentity)]{};
    size_t size = sizeof(identity);
    ESP_ERROR_CHECK(nvs_get_str(nvs, "identity", identity, &size));
    assert(std::strcmp(identity, kIdentity) == 0);

    if (stage == 1) {
        assert(!slot0 && State(running) == ESP_OTA_IMG_PENDING_VERIFY);
        assert(esp_ota_check_rollback_is_possible());
        // There is no network or directory adapter in this executable.
        ESP_ERROR_CHECK(eidolon::ConfirmFirmwareBoot());
        assert(State(running) == ESP_OTA_IMG_VALID);
        Next(nvs, 2);
    }
    if (stage == 2) {
        assert(!slot0 && State(running) == ESP_OTA_IMG_VALID);
        ESP_LOGI("BOOT_TEST", "offline confirmation survives restart PASS");
        Next(nvs, 3, other);
    }
    if (stage == 3) {
        assert(slot0 && State(running) == ESP_OTA_IMG_PENDING_VERIFY);
        assert(esp_ota_check_rollback_is_possible());
        // Inject the startup error, while leaving the real NVS identity intact
        // so a downgrade/erase or accidental data write can be detected.
        eidolon::boot_storage_error = ESP_ERR_NVS_NEW_VERSION_FOUND;
        ESP_ERROR_CHECK(eidolon::ConfirmFirmwareBoot());
        assert(eidolon::boot_storage_error == ESP_ERR_NVS_NEW_VERSION_FOUND);
        assert(State(running) == ESP_OTA_IMG_VALID);
        Next(nvs, 4);
    }
    if (stage == 4) {
        assert(slot0 && State(running) == ESP_OTA_IMG_VALID);
        ESP_LOGI("BOOT_TEST", "storage recovery retains image and identity PASS");
        Next(nvs, 5, other);
    }
    if (stage == 5 && !slot0) {
        assert(State(running) == ESP_OTA_IMG_PENDING_VERIFY);
        ESP_LOGI("BOOT_TEST", "simulate failure before local startup confirmation");
        esp_restart(); // deliberately never confirm this candidate
    }
    assert(stage == 5 && slot0 && State(running) == ESP_OTA_IMG_VALID);
    assert(State(other) == ESP_OTA_IMG_ABORTED);
    nvs_close(nvs);
    ESP_LOGI("BOOT_TEST", "unconfirmed candidate still rolls back PASS");
    ESP_LOGI("BOOT_TEST", "ALL PASS");
}
