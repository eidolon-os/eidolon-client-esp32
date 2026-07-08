#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"
#include "application.h"
#include "button.h"
#include "config.h"

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
    struct TouchExitButtonDriver : public button_driver_t {
        EspBox3Board* board = nullptr;
    };

    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    TouchExitButtonDriver touch_exit_driver_ = {};
    std::unique_ptr<Button> touch_exit_button_;
    bool touch_exit_cached_pressed_ = false;
    int64_t last_touch_poll_us_ = 0;
    uint16_t last_touch_x_ = 0;
    uint16_t last_touch_y_ = 0;

    static constexpr uint16_t kExitHitXMin = DISPLAY_WIDTH - 88;
    static constexpr uint16_t kExitHitYMax = 68;
    static constexpr int64_t kTouchPollIntervalUs = 30 * 1000;

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
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
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

    bool IsExitHotspot(uint16_t x, uint16_t y) const {
        return x >= kExitHitXMin && y <= kExitHitYMax;
    }

    void RequestVoiceLeaveFromTouch() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        ESP_LOGI(TAG, "[ui] EXIT touch x=%u y=%u device_state=%d",
                 last_touch_x_, last_touch_y_, state);
        if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return;
        }

        app.Schedule([]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "[ui] EXIT touch -> RequestVoiceLeave");
                app.RequestVoiceLeave();
            }
        });
    }

    bool ReadTouchExitButtonLevel() {
        int64_t now = esp_timer_get_time();
        if (last_touch_poll_us_ != 0 && now - last_touch_poll_us_ < kTouchPollIntervalUs) {
            return touch_exit_cached_pressed_;
        }
        last_touch_poll_us_ = now;
        touch_exit_cached_pressed_ = false;

        if (touch_ == nullptr || esp_lcd_touch_read_data(touch_) != ESP_OK) {
            return false;
        }

        esp_lcd_touch_point_data_t point = {};
        uint8_t point_count = 0;
        if (esp_lcd_touch_get_data(touch_, &point, &point_count, 1) != ESP_OK || point_count == 0) {
            return false;
        }

        last_touch_x_ = point.x;
        last_touch_y_ = point.y;
        touch_exit_cached_pressed_ = IsExitHotspot(point.x, point.y);
        return touch_exit_cached_pressed_;
    }

    static uint8_t TouchExitButtonGetLevel(button_driver_t* button_driver) {
        auto* driver = static_cast<TouchExitButtonDriver*>(button_driver);
        if (driver == nullptr || driver->board == nullptr) {
            return BUTTON_INACTIVE;
        }
        return driver->board->ReadTouchExitButtonLevel() ? BUTTON_ACTIVE : BUTTON_INACTIVE;
    }

    static esp_err_t TouchExitButtonDelete(button_driver_t* button_driver) {
        (void)button_driver;
        return ESP_OK;
    }

    void InitializeTouchExitButton() {
        touch_exit_driver_.board = this;
        touch_exit_driver_.enable_power_save = false;
        touch_exit_driver_.get_key_level = TouchExitButtonGetLevel;
        touch_exit_driver_.del = TouchExitButtonDelete;

        button_config_t button_config = {
            .long_press_time = 0,
            .short_press_time = 0,
        };
        button_handle_t button_handle = nullptr;
        esp_err_t ret = iot_button_create(&button_config, &touch_exit_driver_, &button_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[ui] touch EXIT virtual button init failed: %s", esp_err_to_name(ret));
            return;
        }

        touch_exit_button_.reset(new Button(button_handle));
        touch_exit_button_->OnPressDown([this]() {
            RequestVoiceLeaveFromTouch();
        });
        ESP_LOGI(TAG, "[ui] touch EXIT virtual button ready");
    }

    void InitializeTouch() {
        if (TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) != ESP_OK &&
            TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP) != ESP_OK &&
            TryInitializeTt21100Touch() != ESP_OK) {
            ESP_LOGW(TAG, "[ui] no Box-3 touch controller detected");
            return;
        }

        InitializeTouchExitButton();
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

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
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
