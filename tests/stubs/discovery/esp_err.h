#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=5, ESP_ERR_INVALID_STATE=1, ESP_ERR_NOT_FOUND=2, ESP_ERR_INVALID_RESPONSE=3, ESP_ERR_NOT_SUPPORTED=4;
inline const char* esp_err_to_name(int){return "stub";}
