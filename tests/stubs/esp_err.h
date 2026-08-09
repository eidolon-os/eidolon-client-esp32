#ifndef TEST_STUB_ESP_ERR_H_
#define TEST_STUB_ESP_ERR_H_

using esp_err_t = int;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_RESPONSE = 0x108;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;

#endif
