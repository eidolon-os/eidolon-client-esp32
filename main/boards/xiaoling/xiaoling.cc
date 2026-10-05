#include "wifi_board.h"
#include "button.h"
#include "config.h"
#include "lcd_init.h"
#include "wake_frame.h"
#include "codecs/no_audio_codec.h"
#include "display/companion_lcd_display.h"
#include "application.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/provisioning_window_policy_core.h"
#include <esp_io_expander_tca9554.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>

class Xiaoling : public WifiBoard {
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_io_expander_handle_t expander_ = nullptr;
    CompanionLcdDisplay* display_ = nullptr;
    Button boot_{BOOT_BUTTON_GPIO};
    Button power_{PWR_BUTTON_GPIO, false, 2000};
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    esp_timer_handle_t battery_timer_ = nullptr;
    int low_voltage_samples_ = 0;

    void InitializeDisplay() {
        i2c_master_bus_config_t bus = {};
        bus.i2c_port = I2C_NUM_0;
        bus.sda_io_num = I2C_SDA_IO;
        bus.scl_io_num = I2C_SCL_IO;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &i2c_bus_));
        ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &expander_));
        constexpr uint32_t resets = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1;
        ESP_ERROR_CHECK(esp_io_expander_set_dir(expander_, resets, IO_EXPANDER_OUTPUT));
        for (int level : {1, 0, 1}) {
            ESP_ERROR_CHECK(esp_io_expander_set_level(expander_, resets, level));
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        const spi_bus_config_t spi = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(
            QSPI_PIN_NUM_LCD_PCLK, QSPI_PIN_NUM_LCD_DATA0, QSPI_PIN_NUM_LCD_DATA1,
            QSPI_PIN_NUM_LCD_DATA2, QSPI_PIN_NUM_LCD_DATA3, DISPLAY_WIDTH * 80 * 2);
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &spi, SPI_DMA_CH_AUTO));
        esp_lcd_panel_io_spi_config_t io = {};
        io.cs_gpio_num = QSPI_PIN_NUM_LCD_CS;
        io.dc_gpio_num = -1;
        io.pclk_hz = 3000000;
        io.trans_queue_depth = 10;
        io.lcd_cmd_bits = 32;
        io.lcd_param_bits = 8;
        io.flags.quad_mode = true;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io, &panel_io));
        uint8_t id[4] = {};
        auto result = esp_lcd_panel_io_rx_param(panel_io, (0x0b << 24) | (0x04 << 8), id, sizeof(id));
        ESP_LOGI("Xiaoling", "LCD ID (%s): %02x %02x %02x %02x", esp_err_to_name(result), id[0], id[1], id[2], id[3]);
        // Release the probe IO before recreating it at the operating clock.
        ESP_ERROR_CHECK(esp_lcd_panel_io_del(panel_io));
        io.pclk_hz = 80000000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io, &panel_io));
        st77916_vendor_config_t vendor = {};
        vendor.flags.use_qspi_interface = true;
        if (result == ESP_OK && id[0] == 0 && id[1] == 2 && id[2] == 0x7f && id[3] == 0x7f) {
            vendor.init_cmds = vendor_specific_init_new;
            vendor.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(vendor_specific_init_new[0]);
        }
        esp_lcd_panel_dev_config_t config = {};
        config.reset_gpio_num = -1;
        config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        config.bits_per_pixel = 16;
        config.vendor_config = &vendor;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        // 252x252 fits inside the 360px circle, including the chrome corners.
#if CONFIG_EIDOLON_HUB_MODE
        display_ = new CompanionLcdDisplay(panel_io, panel, 360, 360, 0, 0, false, false, false, 54);
#else
        display_ = new CompanionLcdDisplay(panel_io, panel, 360, 360, 0, 0, false, false, false);
#endif
    }

    void InitializeWakeChip() {
        uart_config_t config = {};
        config.baud_rate = UART_BAUD_RATE;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;
        ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 512, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &config));
        ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD_PIN, UART_RXD_PIN, -1, -1));
        BaseType_t created = xTaskCreate([](void*) {
            xiaoling::WakeFrameParser parser;
            int64_t last_wake = -2000000;
            uint8_t bytes[64];
            for (;;) {
                int count = uart_read_bytes(UART_PORT_NUM, bytes, sizeof(bytes), pdMS_TO_TICKS(200));
                for (int i = 0; i < count; ++i) {
                    if (!parser.Feed(bytes[i])) continue;
                    const auto now = esp_timer_get_time();
                    if (now - last_wake < 2000000) continue;
                    last_wake = now;
                    ESP_LOGI("Xiaoling", "External wake word detected");
                    Application::GetInstance().WakeWordInvoke("xiaoling");
                }
            }
        }, "external_wake", 3072, nullptr, 5, nullptr);
        ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }

    int BatteryMillivolts() {
        int raw = 0, mv = 0;
        if (!calibration_ || adc_oneshot_read(adc_, ADC_CHANNEL_7, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(calibration_, raw, &mv) != ESP_OK) return -1;
        return static_cast<int>(mv * 3.0f / 0.9945f);
    }

    void InitializeBattery() {
        adc_oneshot_unit_init_cfg_t unit = {};
        unit.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc_));
        adc_oneshot_chan_cfg_t channel = {};
        channel.atten = ADC_ATTEN_DB_12;
        channel.bitwidth = ADC_BITWIDTH_DEFAULT;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_, ADC_CHANNEL_7, &channel));
        adc_cali_curve_fitting_config_t cal = {};
        cal.unit_id = ADC_UNIT_1;
        cal.chan = ADC_CHANNEL_7;
        cal.atten = ADC_ATTEN_DB_12;
        cal.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cal, &calibration_) != ESP_OK) {
            ESP_LOGW("Xiaoling", "Battery calibration unavailable");
            return;
        }
        esp_timer_create_args_t timer = {};
        timer.callback = [](void* arg) {
            auto* self = static_cast<Xiaoling*>(arg);
            const int mv = self->BatteryMillivolts();
            if (mv < 0) return;
            if (mv > 3150) self->low_voltage_samples_ = 0;
            else if (mv <= 3000 && ++self->low_voltage_samples_ >= 3) {
                ESP_LOGW("Xiaoling", "Battery cutoff: %d mV", mv);
                gpio_set_level(PWR_Control_PIN, 0);
            }
        };
        timer.arg = this;
        timer.name = "xiaoling_battery";
        ESP_ERROR_CHECK(esp_timer_create(&timer, &battery_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer_, 5000000));
    }

public:
    Xiaoling() {
        ESP_ERROR_CHECK(gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(PWR_Control_PIN, 1));
        InitializeDisplay();
        InitializeBattery();
#if CONFIG_EIDOLON_HUB_MODE
        eidolon::SetEidolonSetupHandler([this]() { EnterWifiConfigMode(); });
        eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::SessionButton, true);
        boot_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (eidolon::HubSetupButtonClickOpensSetup(app.GetDeviceState())) {
                app.Schedule([this]() { EnterWifiConfigMode(); });
            } else {
                eidolon::DispatchEidolonUiInput(eidolon::UiInputSource::SessionButton, eidolon::UiInputGesture::Click);
            }
        });
        boot_.OnLongPress([this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });
#else
        boot_.OnClick([]() { Application::GetInstance().ToggleChatState(); });
#endif
        power_.OnLongPress([]() { gpio_set_level(PWR_Control_PIN, 0); });
        InitializeWakeChip();
        GetBacklight()->RestoreBrightness();
    }
    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex codec(16000, 24000,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_BOTH,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_RIGHT);
        return &codec;
    }
    Display* GetDisplay() override { return display_; }
    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, false);
        return &backlight;
    }
    i2c_master_bus_handle_t GetSharedI2cBus() override { return i2c_bus_; }
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        const int mv = BatteryMillivolts();
        if (mv < 0) return false;
        level = std::clamp((mv - 3000) * 100 / 1120, 0, 100);
        // No charge-detect wire; do not fabricate a charging state.
        charging = false;
        discharging = mv < 4120;
        return true;
    }
};
DECLARE_BOARD(Xiaoling);
