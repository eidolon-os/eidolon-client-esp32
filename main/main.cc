#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "eidolon/owner_trust_storage_policy.h"
#if CONFIG_EIDOLON_HUB_MODE
#include "eidolon/firmware_boot.h"
#endif

#if CONFIG_EIDOLON_AEC_QUALIFICATION
#include "aec_qualification.h"
#endif

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if CONFIG_EIDOLON_HUB_MODE
        // Default NVS may still contain legacy identity and network settings.
        // A version mismatch or full partition is not authorization to erase it.
        ESP_LOGE(TAG, "NVS unavailable (%s); data preserved, explicit recovery required",
                 esp_err_to_name(ret));
#else
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
#endif
    }
#if CONFIG_EIDOLON_HUB_MODE
    if (ret != ESP_OK) {
        eidolon::boot_storage_error = ret;
        ESP_LOGE(TAG, "Storage initialization failed: %s; preserving data", esp_err_to_name(ret));
    }
#else
    ESP_ERROR_CHECK(ret);
#endif

#if CONFIG_EIDOLON_HUB_MODE
    // Owner roots and signed directory checkpoints have a separate,
    // capacity-bounded failure domain. Never erase it automatically: a storage
    // fault must fail closed instead of silently destroying device ownership.
    const esp_partition_t* owner_trust_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
        eidolon::kOwnerTrustPartitionName);
    if (owner_trust_partition != nullptr) {
        // 64 KiB is the budget for new layouts, not a reason to reject an
        // installed smaller NVS partition that IDF can still read and write.
        const esp_err_t trust_result = nvs_flash_init_partition(eidolon::kOwnerTrustPartitionName);
        if (trust_result != ESP_OK) {
            eidolon::boot_storage_error = trust_result;
            ESP_LOGE(TAG, "Owner trust partition unavailable: %s",
                     esp_err_to_name(trust_result));
        } else {
            ESP_LOGI(TAG, "EIDOLON-STORAGE owner_trust=ready bytes=%lu",
                     static_cast<unsigned long>(owner_trust_partition->size));
        }
    } else {
        ESP_LOGW(TAG, "EIDOLON-STORAGE owner_trust=legacy-layout");
    }
    if (eidolon::boot_storage_error != ESP_OK) {
        // Preserve the recovery image too: the previous executable may erase
        // NVS on this fault. Normal startup confirms after board/UI bring-up.
        const auto confirmation = eidolon::ConfirmFirmwareBoot();
        if (confirmation != ESP_OK) {
            ESP_LOGE(TAG, "Could not verify recovery firmware retention: %s",
                     esp_err_to_name(confirmation));
        }
    }
#endif

#if CONFIG_EIDOLON_AEC_QUALIFICATION
    RunAecQualification();
#else
    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
#endif
}
