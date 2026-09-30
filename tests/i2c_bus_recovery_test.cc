#include "boards/common/i2c_bus_recovery.h"
#include <cassert>
#include <cstdio>

namespace {
constexpr int sda = 15, scl = 14;
int lines[16], elapsed, falling_edges, release_after, starts, stops;
bool slave_data_low, slave_clock_low, config_error;
void Reset() {
    for (auto& line : lines) line = 1;
    elapsed = falling_edges = starts = stops = 0;
    release_after = 9;
    slave_data_low = slave_clock_low = config_error = false;
}
}
int gpio_config(const gpio_config_t*) { return config_error ? -1 : ESP_OK; }
int gpio_get_level(gpio_num_t pin) {
    return lines[pin] && !(pin == sda && slave_data_low) && !(pin == scl && slave_clock_low);
}
int gpio_set_level(gpio_num_t pin, int level) {
    if (pin == sda && gpio_get_level(scl)) {
        if (lines[sda] && !level) ++starts;
        if (!lines[sda] && level && !slave_data_low) ++stops;
    }
    if (pin == scl && lines[scl] && !level) {
        ++falling_edges;
        if (falling_edges == release_after) slave_data_low = false;
    }
    lines[pin] = level;
    return ESP_OK;
}
void ets_delay_us(unsigned int us) { elapsed += us; }
int main() {
    Reset(); assert(RecoverI2cBus(sda, scl)); assert(starts == 0 && stops == 0);
    Reset(); slave_data_low = true;
    assert(RecoverI2cBus(sda, scl)); assert(starts == 0 && stops == 1);
    assert(falling_edges == 9 && gpio_get_level(sda) && gpio_get_level(scl));
    Reset(); slave_data_low = true; release_after = 2;
    assert(RecoverI2cBus(sda, scl)); assert(starts == 0 && stops == 1);
    Reset(); slave_clock_low = true;
    assert(!RecoverI2cBus(sda, scl)); assert(elapsed <= 1100 && lines[sda] == 1);
    Reset(); slave_data_low = true; release_after = 99;
    assert(!RecoverI2cBus(sda, scl)); assert(falling_edges <= 10 && lines[sda] == 1);
    Reset(); config_error = true; assert(!RecoverI2cBus(sda, scl));
    puts("i2c_bus_recovery_test: PASS");
}
