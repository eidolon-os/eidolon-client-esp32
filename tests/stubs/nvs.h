#ifndef TEST_STUB_NVS_H_
#define TEST_STUB_NVS_H_

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using nvs_handle_t = std::uint32_t;

constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;

esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, std::uint32_t* value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, std::uint8_t* value);
esp_err_t nvs_get_str(
    nvs_handle_t handle, const char* key, char* value, std::size_t* length);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, std::uint32_t value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, std::uint8_t value);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_commit(nvs_handle_t handle);

#endif
