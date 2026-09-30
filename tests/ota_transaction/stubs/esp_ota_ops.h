#pragma once
#include <cstddef>
using esp_err_t = int;
using esp_ota_handle_t = unsigned;
struct esp_partition_t { size_t size; };
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_INVALID_SIZE=2;
constexpr int ESP_ERR_NOT_SUPPORTED=0x106, ESP_ERR_NOT_FOUND=0x105;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES = static_cast<size_t>(-2);
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);

enum esp_ota_img_states_t { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY };
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
bool esp_ota_check_rollback_is_possible();
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
