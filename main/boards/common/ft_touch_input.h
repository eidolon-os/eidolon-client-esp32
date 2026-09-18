#pragma once
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include "eidolon/eidolon_view.h"

// FT6336 uses the FT5x06 register protocol. Reuse the component driver rather
// than polling coordinates in each board or reimplementing gesture recognition.
inline bool RegisterFtTouchInput(i2c_master_bus_handle_t bus,
                                 int width, int height) {
    esp_lcd_panel_io_handle_t io=nullptr;
    esp_lcd_touch_handle_t touch=nullptr;
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz=400000;
    if (esp_lcd_new_panel_io_i2c(bus,&io_config,&io)!=ESP_OK) return false;
    esp_lcd_touch_config_t config={};
    config.x_max=width;config.y_max=height;
    config.rst_gpio_num=GPIO_NUM_NC;config.int_gpio_num=GPIO_NUM_NC;
    if (esp_lcd_touch_new_i2c_ft5x06(io,&config,&touch)!=ESP_OK) {
        esp_lcd_panel_io_del(io);return false;
    }
    const lvgl_port_touch_cfg_t input={.disp=lv_display_get_default(),.handle=touch};
    // esp_lvgl_port owns its lock internally.
    if (!lvgl_port_add_touch(&input)) {
        esp_lcd_touch_del(touch);esp_lcd_panel_io_del(io);return false;
    }
    eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::Touch,true);
    return true;
}
