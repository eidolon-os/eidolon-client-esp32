#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/companion_lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include <esp_lvgl_port.h>
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"
#include "application.h"
#include "button.h"
#include "config.h"
#if CONFIG_EIDOLON_HUB_MODE
#include "eidolon/provisioning_window_policy_core.h"
#include "eidolon/eidolon_view.h"
#endif

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#include <memory>

#define TAG "EspBox3Board"

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},

    {0, (uint8_t []){0}, 0xff, 0},
};

class EspBox3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_6;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_7;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
#if CONFIG_EIDOLON_HUB_MODE
        eidolon::SetEidolonSetupHandler([this]() { EnterWifiConfigMode(); });
        eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::SessionButton,true);
#endif
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
#if CONFIG_EIDOLON_HUB_MODE
            if (eidolon::HubSetupButtonClickOpensSetup(app.GetDeviceState())) {
#else
            if (app.GetDeviceState() == kDeviceStateStarting) {
#endif
                // Button callbacks run on esp_timer. Physical recovery can
                // generate keys and write NVS; keep it on the application task.
                app.Schedule([this]() { EnterWifiConfigMode(); });
                return;
            }
#if CONFIG_EIDOLON_HUB_MODE
            eidolon::DispatchEidolonUiInput(eidolon::UiInputSource::SessionButton,eidolon::UiInputGesture::Click);
#else
            app.ToggleChatState();
#endif
        });

        // Long-press also opens setup from an operational device.
        // The board adapter owns the gesture; the common Wi-Fi/Owner
        // provisioning service continues to own the bounded setup act itself.
        boot_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                EnterWifiConfigMode();
            });
        });
    }

    esp_lcd_touch_config_t CreateTouchConfig(bool tt21100) {
        return {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = true,
                .mirror_y = tt21100 ? false : DISPLAY_MIRROR_Y,
            },
        };
    }

    esp_lcd_panel_io_i2c_config_t CreateTouchIoConfig(uint32_t address) {
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = address;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 0;
        io_config.lcd_cmd_bits = 16;
        io_config.flags.disable_control_phase = 1;
        io_config.scl_speed_hz = 400 * 1000;
        return io_config;
    }

    esp_err_t TryInitializeGt911Touch(uint8_t address) {
        esp_lcd_panel_io_handle_t touch_io = nullptr;
        auto io_config = CreateTouchIoConfig(address);

        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ui] GT911 touch IO 0x%02x init failed: %s", address, esp_err_to_name(ret));
            return ret;
        }

        auto touch_config = CreateTouchConfig(false);
        ret = esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ui] GT911 touch 0x%02x init failed: %s", address, esp_err_to_name(ret));
            esp_lcd_panel_io_del(touch_io);
            return ret;
        }

        ESP_LOGI(TAG, "[ui] GT911 touch ready addr=0x%02x", address);
        return ESP_OK;
    }

    esp_err_t TryInitializeTt21100Touch() {
        esp_lcd_panel_io_handle_t touch_io = nullptr;
        auto io_config = CreateTouchIoConfig(ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS);

        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ui] TT21100 touch IO init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        auto touch_config = CreateTouchConfig(true);
        ret = esp_lcd_touch_new_i2c_tt21100(touch_io, &touch_config, &touch_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ui] TT21100 touch init failed: %s", esp_err_to_name(ret));
            esp_lcd_panel_io_del(touch_io);
            return ret;
        }

        ESP_LOGI(TAG, "[ui] TT21100 touch ready");
        return ESP_OK;
    }

    void InitializeTouch() {
#if CONFIG_EIDOLON_HUB_MODE && !CONFIG_EIDOLON_UI_TOUCH_CONTROLS
        // Display-only is an explicit product choice, not inferred from duplex.
        return;
#endif
        if (TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) != ESP_OK &&
            TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP) != ESP_OK &&
            TryInitializeTt21100Touch() != ESP_OK) {
            ESP_LOGW(TAG, "[ui] no Box-3 touch controller detected");
            return;
        }

        const lvgl_port_touch_cfg_t touch_config = {
            .disp = lv_display_get_default(),
            .handle = touch_,
        };
        if (lvgl_port_add_touch(&touch_config) == nullptr) {
            ESP_LOGE(TAG, "[ui] LVGL touch registration failed");
        } else {
            ESP_LOGI(TAG, "[ui] LVGL touch ready");
#if CONFIG_EIDOLON_HUB_MODE
            eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::Touch,true);
#endif
        }
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_5;
        io_config.dc_gpio_num = GPIO_NUM_4;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_48;
        panel_config.flags.reset_active_high = 1,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new CompanionLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    }

public:
    EspBox3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeIli9341Display();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspBox3Board);
