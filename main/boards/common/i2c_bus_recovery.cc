#include "i2c_bus_recovery.h"

#include <esp_log.h>
#include <rom/ets_sys.h>

#define TAG "I2cBusRecovery"

namespace {

// 100kHz, the slowest standard rate, so a slave that is slow to release still
// sees the clocks it is waiting for.
constexpr int kHalfPeriodUs = 5;
constexpr int kClockReleaseTimeoutUs = 1000;

bool ReleaseClock(gpio_num_t scl)
{
    gpio_set_level(scl, 1);
    for (int elapsed = 0; elapsed < kClockReleaseTimeoutUs; elapsed += kHalfPeriodUs) {
        ets_delay_us(kHalfPeriodUs);
        if (gpio_get_level(scl) == 1) return true;
    }
    return false;
}

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
    if (!ReleaseClock(scl)) {
        ESP_LOGE(TAG, "SCL is held low; bus recovery deferred");
        return false;
    }
    if (gpio_get_level(sda) == 1) return true;

    ESP_LOGW(TAG, "SDA is held low; clocking the bus free");
    // Sample SDA while SCL is low, as IDF's software bus-clear does. A slave
    // transmitting zero bytes only releases SDA for the master's ACK/NACK.
    Drive(scl);
    for (int pulse = 0; pulse < 9 && gpio_get_level(sda) == 0; ++pulse) {
        if (!ReleaseClock(scl)) {
            Release(sda);
            ESP_LOGE(TAG, "SCL remained low during bus recovery");
            return false;
        }
        Drive(scl);
    }

    // Set SDA low while SCL is LOW, then release SCL and finally SDA. Pulling
    // SDA down with SCL already high generates START, not the setup for STOP.
    Drive(sda);
    if (!ReleaseClock(scl)) {
        Release(sda);
        return false;
    }
    Release(sda);
    const bool released = gpio_get_level(sda) == 1 && gpio_get_level(scl) == 1;
    if (!released) ESP_LOGE(TAG, "I2C bus still held after STOP: SDA=%d SCL=%d",
                           gpio_get_level(sda), gpio_get_level(scl));
    return released;
}
