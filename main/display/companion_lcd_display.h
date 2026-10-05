#pragma once
#include "lcd_display.h"
#if CONFIG_EIDOLON_HUB_MODE
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "eidolon/views/companion_face_view.h"
class LvglFont;
// One product display implementation for BOX-3, StackChan and CoreS3.
// The SPI driver and font/theme infrastructure remain shared with LcdDisplay.
class CompanionLcdDisplay : public SpiLcdDisplay {
public:
    CompanionLcdDisplay(esp_lcd_panel_io_handle_t io,esp_lcd_panel_handle_t panel,
        int width,int height,int ox,int oy,bool mx,bool my,bool swap,int safe_inset=0);
    ~CompanionLcdDisplay() override;
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void OnAssetsLoaded() override;
    void OnAssetsUnloaded() override;
    void SetStatus(const char* text) override { face_.SetSystemStatus(text); }
    void ShowNotification(const char* text,int duration_ms=3000) override { face_.ShowNotification(text,duration_ms); }
    void ShowNotification(const std::string& text,int duration_ms=3000) override { ShowNotification(text.c_str(),duration_ms); }
    // Companion content comes from the projected UI model / typed plans only.
    void SetEmotion(const char*) override {}
    void SetChatMessage(const char*,const char*) override {}
    void ClearChatMessages() override {}
    void PulseAvatar(const char* emotion,int ttl_ms) { face_.PulseEmotion(emotion,ttl_ms); }
private:
    static bool OnTransfer(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t*,void*);
    static void OnFlush(lv_event_t* event);
    static void OnTimer(lv_timer_t* timer);
    int safe_inset_=0;
    eidolon::CompanionFaceView face_;
    std::shared_ptr<LvglFont> fallback_font_;
    std::shared_ptr<LvglFont> content_font_;
    lv_timer_t* timer_=nullptr;
    portMUX_TYPE frame_mux_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t submitted_frame_=0,completed_frame_=0;
};
#else
using CompanionLcdDisplay = SpiLcdDisplay;
#endif
