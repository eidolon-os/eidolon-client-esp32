#include "companion_lcd_display.h"
#include "lvgl_theme.h"
#include "eidolon/eidolon_view.h"
#include <esp_check.h>
CompanionLcdDisplay::CompanionLcdDisplay(esp_lcd_panel_io_handle_t io,esp_lcd_panel_handle_t panel,
    int width,int height,int ox,int oy,bool mx,bool my,bool swap)
    : SpiLcdDisplay(io,panel,width,height,ox,oy,mx,my,swap,false,10) {
    // Same completion contract as esp_lvgl_port, plus a frame completion stamp.
    // No LVGL object access, allocation or network work in the SPI ISR.
    const esp_lcd_panel_io_callbacks_t callbacks={.on_color_trans_done=OnTransfer};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io,&callbacks,this));
    lv_display_add_event_cb(display_,OnFlush,LV_EVENT_FLUSH_START,this);
}
CompanionLcdDisplay::~CompanionLcdDisplay() {
    if (timer_) lv_timer_delete(timer_);
    eidolon::SetEidolonView(nullptr);
}
bool CompanionLcdDisplay::OnTransfer(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t*,void* ctx) {
    auto* self=static_cast<CompanionLcdDisplay*>(ctx);
    if (lv_display_flush_is_last(self->display_)) {
        portENTER_CRITICAL_ISR(&self->frame_mux_);
        self->completed_frame_=self->submitted_frame_;
        portEXIT_CRITICAL_ISR(&self->frame_mux_);
    }
    lv_display_flush_ready(self->display_);
    return false;
}
void CompanionLcdDisplay::OnFlush(lv_event_t* event) {
    auto* self=static_cast<CompanionLcdDisplay*>(lv_event_get_user_data(event));
    if (lv_display_flush_is_last(self->display_)) {
        portENTER_CRITICAL(&self->frame_mux_);
        self->submitted_frame_=self->face_.prepared_frame();
        portEXIT_CRITICAL(&self->frame_mux_);
    }
}
void CompanionLcdDisplay::OnTimer(lv_timer_t* timer) {
    auto* self=static_cast<CompanionLcdDisplay*>(lv_timer_get_user_data(timer));
    portENTER_CRITICAL(&self->frame_mux_);
    const auto completed=self->completed_frame_;
    portEXIT_CRITICAL(&self->frame_mux_);
    self->face_.Advance(completed);
}
void CompanionLcdDisplay::SetupUI() {
    if (setup_ui_called_) return;
    Display::SetupUI();
    DisplayLockGuard lock(this);
    auto* theme=static_cast<LvglTheme*>(current_theme_);
    auto* screen=lv_display_get_screen_active(display_);
    // Use the board-sized boot font for chrome, not the legacy 30px UI patch.
    fallback_font_=LvglThemeManager::GetInstance().GetTheme("light")->text_font();
    content_font_=fallback_font_;
    face_.Build({screen,fallback_font_->font(),this,theme->icon_font()->font()});
    // Base service owns only hardware indicators. The view owns lifecycle and
    // notification text, so there is no second status label over the face.
    const auto indicators=face_.indicators();
    mute_label_=indicators.microphone;
    network_label_=indicators.network;
    battery_label_=indicators.battery;
    eidolon::SetEidolonView(&face_);
    timer_=lv_timer_create(OnTimer,20,this);
}
void CompanionLcdDisplay::SetTheme(Theme* theme) {
    // The companion owns its coordinated palette; generic chat theme objects do not
    // exist on this surface. Save the preference through the base contract.
    Display::SetTheme(theme);
}

void CompanionLcdDisplay::OnAssetsLoaded() {
    DisplayLockGuard lock(this);
    if (!fallback_font_) return;
    auto* light=LvglThemeManager::GetInstance().GetTheme("light");
    if (!light || !light->text_font() || !light->text_font()->font()) return;
    // Keep ownership of the descriptor while LVGL labels hold its raw pointer.
    auto font=light->text_font();
    face_.SetContentFont(font->font());
    content_font_=std::move(font);
    ESP_LOGI("CompanionLcdDisplay", "Asset text font applied line_height=%ld",
             static_cast<long>(content_font_->font()->line_height));
}
void CompanionLcdDisplay::OnAssetsUnloaded() {
    DisplayLockGuard lock(this);
    if (!fallback_font_) return;
    face_.SetContentFont(fallback_font_->font());
    // A replacement resource pack may omit a font. Do not leave the theme
    // pointing into an unmapped pack for the next OnAssetsLoaded callback.
    for (const char* name : {"light", "dark"}) {
        auto* theme=LvglThemeManager::GetInstance().GetTheme(name);
        if (theme && theme->text_font()==content_font_) theme->set_text_font(fallback_font_);
    }
    content_font_=fallback_font_;
}
