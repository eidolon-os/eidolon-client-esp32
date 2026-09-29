#include "wifi_board.h"
#include "codecs/es8389_audio_codec.h"
#include "display/display.h"
#include "display/lcd_display.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt1151.h"
#include "application.h"
#include "button.h"
// The ADC-ladder key voltages in config.h name their channel through
// ADC1_GPIO42_CHANNEL, which lives here rather than in button_adc.h.
#include <soc/adc_channel.h>
#include "config.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>

#if CONFIG_EIDOLON_HUB_MODE
#include "lvgl_theme.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/provisioning_window_policy_core.h"
#include "eidolon/views/home_panel_view.h"

#include <memory>
#endif

#define TAG "Korvo1Board"

#if CONFIG_EIDOLON_HUB_MODE
// The RGB display with the smart home panel as its only surface. The generic
// chat widgets LcdDisplay builds are hidden, not deleted: hidden objects never
// invalidate, which matters on a panel that repaints in full on every change.
class Korvo1PanelDisplay : public RgbLcdDisplay {
public:
    using RgbLcdDisplay::RgbLcdDisplay;

    ~Korvo1PanelDisplay() override { eidolon::SetEidolonView(nullptr); }

    void SetupUI() override {
        if (setup_ui_called_) return;
        RgbLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        for (lv_obj_t* legacy : {container_, emoji_box_, preview_image_, top_bar_, status_bar_, bottom_bar_,
                                 low_battery_popup_}) {
            if (legacy != nullptr) lv_obj_add_flag(legacy, LV_OBJ_FLAG_HIDDEN);
        }
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        // The panel is drawn in 20 px. eidolon_dark's text font is the 30 px
        // supplement other boards put in front of this base, which mixed 30 px
        // characters into the panel's text and set its line height; the light
        // theme holds the base itself.
        auto* light = LvglThemeManager::GetInstance().GetTheme("light");
        builtin_font_ = light != nullptr && light->text_font() ? light->text_font() : theme->text_font();
        font_ = builtin_font_;
        panel_.Build({lv_screen_active(), this, font_->font(), theme->icon_font()->font()});
        eidolon::SetEidolonView(&panel_);
    }

    // The assets partition carries the full-coverage text font; the builtin
    // one only has to get the device through boot.
    void OnAssetsLoaded() override {
        DisplayLockGuard lock(this);
        auto* light = LvglThemeManager::GetInstance().GetTheme("light");
        if (!builtin_font_ || light == nullptr || !light->text_font() || light->text_font()->font() == nullptr) {
            return;
        }
        auto font = light->text_font();
        panel_.SetFont(font->font());
        font_ = std::move(font);  // labels hold the raw pointer; keep it alive
    }

    void OnAssetsUnloaded() override {
        DisplayLockGuard lock(this);
        if (!builtin_font_) return;
        panel_.SetFont(builtin_font_->font());
        for (const char* name : {"light", "dark"}) {
            auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
            if (theme != nullptr && theme->text_font() == font_) theme->set_text_font(builtin_font_);
        }
        font_ = builtin_font_;
    }

    void ShowNotification(const char* text, int duration_ms = 3000) override {
        panel_.ShowNotification(text, duration_ms);
    }
    // The legacy status bar is hidden, so the network state it used to show is
    // handed to the panel's top bar instead, on the same 10-second cadence
    // (Application calls this every second from its main loop).
    void UpdateStatusBar(bool update_all = false) override {
        if (!update_all && status_ticks_++ % 10 != 0) return;
        panel_.SetNetworkIcon(Board::GetInstance().GetNetworkStateIcon());
    }
    void ShowNotification(const std::string& text, int duration_ms = 3000) override {
        ShowNotification(text.c_str(), duration_ms);
    }
    // Lifecycle text reaches the panel through the projected UI model only; the
    // hidden chat widgets must stay hidden (and their scroll animations idle).
    void SetStatus(const char*) override {}
    void SetEmotion(const char*) override {}
    void SetChatMessage(const char*, const char*) override {}
    void ClearChatMessages() override {}

private:
    eidolon::HomePanelView panel_;
    unsigned status_ticks_ = 0;
    std::shared_ptr<LvglFont> builtin_font_;
    std::shared_ptr<LvglFont> font_;
};
using Korvo1Display = Korvo1PanelDisplay;
#else
using Korvo1Display = RgbLcdDisplay;
#endif

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
    adc_oneshot_unit_handle_t button_adc_ = nullptr;
    AdcButton* set_button_ = nullptr;
    AdcButton* mode_button_ = nullptr;
    AdcButton* volume_up_button_ = nullptr;
    AdcButton* volume_down_button_ = nullptr;

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
    // S31 AXI DMA reads the PSRAM framebuffers directly, matching Espressif's
    // esp32_s31_korvo1 board configuration. A bounce buffer would require the
    // CPU to refill every 10 lines; delayed EOF interrupts can leave its software
    // cursor out of step with scanout. IDF's bounce restart is S3-only.
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
            .bounce_buffer_size_px = 0,
            // As in Espressif's own Korvo-1 board definition (esp_boards,
            // esp-claw): 64-byte bursts keep the PSRAM reads of scan-out short.
            .dma_burst_size = 64,
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
        ESP_LOGI(TAG, "RGB scanout: direct PSRAM DMA, 2 framebuffers, pclk=%d Hz",
                 DISPLAY_PCLK_HZ);

        display_ = new Korvo1Display(nullptr, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // The four keys share one ADC ladder, so each is a window around its
    // centre rather than a pin. Every button passes a null adc_handle: the
    // driver keeps one unit per ADC and the first device to ask creates it.
    AdcButton* MakeAdcButton(int index, int min_mv, int max_mv) {
        button_adc_config_t cfg = {};
        cfg.adc_handle = &button_adc_;
        cfg.unit_id = BUTTON_ADC_UNIT;
        cfg.adc_channel = BUTTON_ADC_CHANNEL;
        cfg.button_index = index;
        cfg.min = min_mv;
        cfg.max = max_mv;
        return new AdcButton(cfg);
    }

    void InitializeButtons() {
        // The unit is ours so the ladder can be read back directly; the button
        // driver still configures the channel, and its attenuation is what any
        // reading has to agree with.
        adc_oneshot_unit_init_cfg_t adc_cfg = {};
        adc_cfg.unit_id = BUTTON_ADC_UNIT;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &button_adc_));

        set_button_ = MakeAdcButton(0, BUTTON_ADC_SET_MIN_MV, BUTTON_ADC_SET_MAX_MV);
        mode_button_ = MakeAdcButton(1, BUTTON_ADC_MODE_MIN_MV, BUTTON_ADC_MODE_MAX_MV);
        volume_up_button_ =
            MakeAdcButton(2, BUTTON_ADC_VOL_UP_MIN_MV, BUTTON_ADC_VOL_UP_MAX_MV);
        volume_down_button_ =
            MakeAdcButton(3, BUTTON_ADC_VOL_DOWN_MIN_MV, BUTTON_ADC_VOL_DOWN_MAX_MV);

        // SET follows the Box-3 setup and session gestures. Button callbacks
        // run on the esp_timer task:
        // physical recovery generates keys and writes NVS, so the setup act is
        // scheduled onto the application task. Calling it here directly
        // overflowed the timer task and rebooted the board on every long press,
        // which left a removed ("recovery required") panel no way back in.
#if CONFIG_EIDOLON_HUB_MODE
        eidolon::SetEidolonSetupHandler([this]() { EnterWifiConfigMode(); });
        eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::SessionButton, true);
#endif
        set_button_->OnClick([this]() {
            auto& app = Application::GetInstance();
#if CONFIG_EIDOLON_HUB_MODE
            if (eidolon::HubSetupButtonClickOpensSetup(app.GetDeviceState())) {
#else
            if (app.GetDeviceState() == kDeviceStateStarting) {
#endif
                app.Schedule([this]() { EnterWifiConfigMode(); });
                return;
            }
#if CONFIG_EIDOLON_HUB_MODE
            eidolon::DispatchEidolonUiInput(eidolon::UiInputSource::SessionButton,
                                            eidolon::UiInputGesture::Click);
#else
            app.ToggleChatState();
#endif
        });
        // Long press opens setup from an operational or removed device too.
        set_button_->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); });
        });

#if CONFIG_EIDOLON_HUB_MODE
        // MODE offers the same conversation gesture through the shared UI input.
        mode_button_->OnClick([]() {
            eidolon::DispatchEidolonUiInput(eidolon::UiInputSource::SessionButton,
                                            eidolon::UiInputGesture::Click);
        });
#elif CONFIG_USE_DEVICE_AEC
        mode_button_->OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        volume_up_button_->OnClick([this]() { ChangeVolume(10); });
        volume_up_button_->OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });
        volume_down_button_->OnClick([this]() { ChangeVolume(-10); });
        volume_down_button_->OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

    }


    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    // Touch is the smart home panel's input, but no provisioning or lifecycle
    // flow depends on it, so a GT1151 that does not answer is logged and
    // stepped over rather than fatal.
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

        const lvgl_port_touch_cfg_t touch_config = {
            .disp = lv_display_get_default(),
            .handle = touch_,
        };
        if (lvgl_port_add_touch(&touch_config) == nullptr) {
            ESP_LOGE(TAG, "[ui] LVGL touch registration failed");
            return;
        }
        ESP_LOGI(TAG, "[ui] LVGL touch ready");
#if CONFIG_EIDOLON_HUB_MODE
        eidolon::SetEidolonInputAvailable(eidolon::UiInputSource::Touch, true);
#endif
    }

public:
    Korvo1Board() {
        InitializeI2c();
        InitializeRgbDisplay();
        InitializeTouch();
        InitializeButtons();
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
            AUDIO_INPUT_REFERENCE,
            AUDIO_CODEC_INPUT_GAIN_DB);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(Korvo1Board);
