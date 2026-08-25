#include "i2c_bus_recovery.h"

#include <esp_log.h>
#include <rom/ets_sys.h>

#define TAG "I2cBusRecovery"

namespace {

// 100kHz, the slowest standard rate, so a slave that is slow to release still
// sees the clocks it is waiting for.
constexpr int kHalfPeriodUs = 5;

void Release(gpio_num_t pin)
{
    gpio_set_level(pin, 1);
    ets_delay_us(kHalfPeriodUs);
}

void Drive(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
    ets_delay_us(kHalfPeriodUs);
}

}  // namespace

bool RecoverI2cBus(gpio_num_t sda, gpio_num_t scl)
{
    // Open drain with pull-ups, which is what the bus is: a line is "released"
    // by letting the pull-up take it high, never by driving it high.
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&pins) != ESP_OK) {
        return false;
    }
    Release(sda);
    Release(scl);
    if (gpio_get_level(sda) == 1) {
        return true;  // Nobody is holding it.
    }

    ESP_LOGW(TAG, "SDA is held low; clocking the bus free");
    for (int pulse = 0; pulse < 9 && gpio_get_level(sda) == 0; ++pulse) {
        Drive(scl);
        Release(scl);
    }
    const bool released = gpio_get_level(sda) == 1;

    // STOP: SDA rises while SCL is high, which is what tells every slave the
    // transfer is over and leaves the bus idle for the peripheral that follows.
    Drive(sda);
    Release(scl);
    Release(sda);

    if (!released) {
        ESP_LOGE(TAG, "SDA is still held low after nine clocks");
    }
    return released;
}
