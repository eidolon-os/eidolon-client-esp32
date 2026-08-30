#include "wifi_board.h"
#include "codecs/es8389_audio_codec.h"
#include "display/display.h"
#include "display/lcd_display.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt1151.h"
#include "application.h"
#include "config.h"

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <driver/i2c_master.h>

#define TAG "Korvo1Board"

// ESP32-S31-Korvo-1.
//
// Brought up as the esp-box-3 equivalent on the new silicon: full duplex with a
// device-side AEC reference. The reference comes from the ES8389 itself
// (ADCL = mic, DACR = playback loopback) rather than from a separate ADC as on
// box-3, but the capture topology the AFE sees is identical — one mic plus one
// reference.
class Korvo1Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Display* display_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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

    // ST7262E43 is a plain RGB TFT driver: there is no vendor register set to
    // push, so the panel is created straight from the timing block. The
    // framebuffers live in PSRAM and are fed through a small internal-SRAM
    // bounce buffer, which is how the RGB peripheral is meant to be driven —
    // unlike a SPI panel, where an LVGL draw buffer in PSRAM would break.
    void InitializeRgbDisplay() {
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_rgb_panel_config_t rgb_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .timings = {
                .pclk_hz = DISPLAY_PCLK_HZ,
                .h_res = DISPLAY_WIDTH,
                .v_res = DISPLAY_HEIGHT,
                .hsync_pulse_width = DISPLAY_HSYNC_PULSE_WIDTH,
                .hsync_back_porch = DISPLAY_HSYNC_BACK_PORCH,
                .hsync_front_porch = DISPLAY_HSYNC_FRONT_PORCH,
                .vsync_pulse_width = DISPLAY_VSYNC_PULSE_WIDTH,
                .vsync_back_porch = DISPLAY_VSYNC_BACK_PORCH,
                .vsync_front_porch = DISPLAY_VSYNC_FRONT_PORCH,
                .flags = {
                    .pclk_active_neg = true,
                },
            },
            // ESP-IDF 6 dropped bits_per_pixel from this struct; the bus width
            // alone describes an RGB565 panel.
            .data_width = 16,
            .num_fbs = 2,
            .bounce_buffer_size_px = DISPLAY_WIDTH * 10,
            .hsync_gpio_num = DISPLAY_LCD_HSYNC,
            .vsync_gpio_num = DISPLAY_LCD_VSYNC,
            .de_gpio_num = DISPLAY_LCD_DE,
            .pclk_gpio_num = DISPLAY_LCD_PCLK,
            .disp_gpio_num = DISPLAY_LCD_DISP,
            .data_gpio_nums = {
                DISPLAY_LCD_DATA0, DISPLAY_LCD_DATA1, DISPLAY_LCD_DATA2, DISPLAY_LCD_DATA3,
                DISPLAY_LCD_DATA4, DISPLAY_LCD_DATA5, DISPLAY_LCD_DATA6, DISPLAY_LCD_DATA7,
                DISPLAY_LCD_DATA8, DISPLAY_LCD_DATA9, DISPLAY_LCD_DATA10, DISPLAY_LCD_DATA11,
                DISPLAY_LCD_DATA12, DISPLAY_LCD_DATA13, DISPLAY_LCD_DATA14, DISPLAY_LCD_DATA15,
            },
            .flags = {
                .fb_in_psram = 1,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

        display_ = new RgbLcdDisplay(nullptr, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // Touch is not required for any Eidolon flow on this board, so a GT1151
    // that does not answer is logged and stepped over rather than fatal.
    void InitializeTouch() {
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle) != ESP_OK) {
            ESP_LOGW(TAG, "[ui] GT1151 IO create failed; continuing without touch");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = DISPLAY_TOUCH_RST_PIN,
            .int_gpio_num = DISPLAY_TOUCH_INT_PIN,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        };
        esp_err_t err = esp_lcd_touch_new_i2c_gt1151(tp_io_handle, &tp_cfg, &touch_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[ui] GT1151 init failed: %s; continuing without touch",
                     esp_err_to_name(err));
            touch_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "[ui] GT1151 touch ready");
    }

public:
    Korvo1Board() {
        InitializeI2c();
        InitializeRgbDisplay();
        InitializeTouch();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8389_ADDR,
            AUDIO_CODEC_USE_MCLK,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(Korvo1Board);
