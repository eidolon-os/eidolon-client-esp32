#pragma once
#include <cstdint>
using gpio_num_t = int;
constexpr int ESP_OK = 0;
constexpr int GPIO_MODE_INPUT_OUTPUT_OD = 3;
constexpr int GPIO_PULLUP_ENABLE = 1;
constexpr int GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; int pull_down_en; int intr_type; };
int gpio_config(const gpio_config_t*);
int gpio_set_level(gpio_num_t, int);
int gpio_get_level(gpio_num_t);
