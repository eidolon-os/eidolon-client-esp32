#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t*, size_t, int);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
inline const char* esp_err_to_name(esp_err_t) { return "test-error"; }
class Axp2101 {
protected:
    i2c_master_dev_handle_t i2c_device_;
public:
    Axp2101(i2c_master_bus_handle_t bus, uint8_t) : i2c_device_(bus) {}
};
