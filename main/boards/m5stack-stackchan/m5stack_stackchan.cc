#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "i2c_device.h"
#include "axp2101.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include <cstring>
#include "esp_video.h"
#include "stackchan_body.h"

#if CONFIG_EIDOLON_HUB_MODE
#include "display/lvgl_display/lvgl_theme.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/views/stackchan_avatar_view.h"
#include <lvgl.h>
#endif

#define TAG "M5StackChanBoard"

// The DVP camera's internal/DMA SRAM starves the full_duplex AFE voice
// capturer: on room.join the mic-path AFE read task (8 KB internal stack)
// fails to allocate (internal-SRAM low-water ~9 KB), the voice room never
// forms, and the control room can't rebuild either → device goes offline.
// esp-box-3 (the full_duplex reference) has no camera. Disabled here until
// internal-SRAM optimization (buffers→PSRAM, control→voice teardown sync)
// lets the camera coexist. Flip to 1 to re-enable.
#define EIDOLON_STACKCHAN_ENABLE_CAMERA 0

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

// CoreS3 display that mounts the StackChan expressive avatar as the eidolon view.
// The presenter (eidolon_ui_presenter) renders session state to whatever view the
// board registers via SetEidolonView(); here that is the ported StackChan face.
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy)
        // draw_buffer_psram=true: StackChan runs the on-device AFE full_duplex
        // mic path and is internal-SRAM tight (servo/motion tasks + camera cost
        // that box3 doesn't pay), so move the LVGL draw buffer to PSRAM to free
        // ~12.8 KB internal for the AFE. The slow-updating avatar face tolerates
        // the slightly slower flush.
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy, /*draw_buffer_psram=*/true) {}

#if CONFIG_EIDOLON_HUB_MODE
    void SetupUI() override {
        // Base creates the standard LVGL objects first; the avatar panel is then
        // added on top of the active screen (covers the full 320x240 face).
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        eidolon::StackChanAvatarView::BuildContext ctx;
        ctx.parent = lv_screen_active();
        ctx.font = static_cast<LvglTheme*>(current_theme_)->text_font()->font();
        ctx.display = this;
        avatar_view_ = new eidolon::StackChanAvatarView();
        avatar_view_->Build(ctx);
        eidolon::SetEidolonView(avatar_view_);
    }

    // Transient face pulse (owner-presence reflex). Forwards to the avatar view.
    void PulseAvatar(const char* emotion, int ttl_ms) {
        if (avatar_view_) avatar_view_->PulseEmotion(emotion, ttl_ms);
    }

private:
    eidolon::StackChanAvatarView* avatar_view_ = nullptr;
#endif
};

class M5StackChanBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LcdDisplay* display_;
    EspVideo* camera_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    StackChanBody* body_ = nullptr;

    // No PowerSaveTimer: StackChan is a full_duplex always-on companion, so it
    // must never dim/sleep or auto-power-off (matches the esp-box-3 reference).
    // The stock M5Stack timer both shut the device off at 300s idle AND had a
    // change-detection bug that fired even on USB power, so it is removed here.

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

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按
        const int64_t LONG_PRESS_WIFI_CONFIG_MS = 2000;  // 长按≥2s：任意状态进 WiFi 配网
        
        ft6336_->UpdateTouchPoint();
        auto& touch_point = ft6336_->GetTouchPoint();
        
        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        } 
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            
            // 短触：切换对话。进配网走下面的长按，任意状态都可用，不再依赖
            // 开机那几秒。
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                Application::GetInstance().ToggleChatState();
            } else if (touch_duration >= LONG_PRESS_WIFI_CONFIG_MS) {
                // 长按 >=2s：任意状态下进入 WiFi 配网。这是 CoreS3 的可靠配网入口，
                // 不依赖开机那几秒的短触窗口（其他板子有 boot 按键，CoreS3 没有）。
                ESP_LOGI(TAG, "Long-press detected: entering WiFi config mode");
                StartWifiConfigMode();
                return;
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackChanBoard* board = (M5StackChanBoard*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new CustomLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
        camera_->SetHMirror(false);
    }

    // Bring up the StackChan servo body (yaw/pitch). If the servo base is absent or the
    // IO-expander does not answer, the body stays disabled and the board runs as a plain
    // screen-avatar CoreS3 — the rest of the device is unaffected.
    void InitializeServoBody() {
        ESP_LOGI(TAG, "Init StackChan servo body");
        body_ = new StackChanBody(i2c_bus_);
        if (body_->Init()) {
            // Boot bring-up sweep intentionally NOT run (owner request: no automatic
            // servo motion on boot). Init() already homes the head to its rest pose;
            // StartSelfTest() stays available for manual bring-up verification.
        } else {
            ESP_LOGW(TAG, "Servo body unavailable; continuing without head motion");
        }
    }

public:
    M5StackChanBoard() {
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
#if EIDOLON_STACKCHAN_ENABLE_CAMERA
        InitializeCamera();
#endif
        InitializeFt6336TouchPad();
        InitializeServoBody();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }

    // Head/body motion: delegate to the servo body (no-op if the base is absent).
    bool HasHeadMotion() override { return body_ != nullptr && body_->ready(); }
    void HeadLookAt(float x, float y, int speed, int ttl_ms) override {
        if (body_) body_->LookAtNormalized(x, y, speed, ttl_ms);
    }
    void HeadHome() override {
        if (body_) body_->GoHome();
    }
    void HeadGesture(const std::string& name, int times, float x, float y,
                     int hold_ms, int return_ms) override {
        if (body_) body_->HeadGesture(name, times, x, y, hold_ms, return_ms);
    }
    void HeadStop() override {
        if (body_) body_->Stop();
    }
    // Mic-capture noise gate: while the mic is hot, cut the servo power rail so its
    // switching whine can't corrupt the uplink (head goes limp during capture).
    void SetCaptureQuiet(bool quiet) override {
        if (body_) body_->SetCaptureQuiet(quiet);
    }
    void RgbEffect(const char* effect) override {
        if (!body_) return;
        if (effect != nullptr && std::strcmp(effect, "off") == 0) {
            body_->RgbOff();
        } else {
            body_->RgbMarquee();  // "wake" / default
        }
    }
    void AvatarExpress(const char* emotion, int ttl_ms) override {
#if CONFIG_EIDOLON_HUB_MODE
        if (display_) static_cast<CustomLcdDisplay*>(display_)->PulseAvatar(emotion, ttl_ms);
#else
        (void)emotion; (void)ttl_ms;
#endif
    }
};

DECLARE_BOARD(M5StackChanBoard);
