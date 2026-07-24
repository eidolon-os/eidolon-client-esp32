#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEidolonLifecycle(const char* state, const char* detail) override;
    virtual void SetVoiceChrome(const char* mode, const char* state, const char* action,
                                bool action_visible) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);
    void OnAssetsUnloaded();
    void OnAssetsLoaded();

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    emote_handle_t emote_handle_ = nullptr;
    bool assets_loaded_ = false;
    std::string pending_emotion_ = "idle";
    std::string pending_chat_role_ = "system";
    std::string pending_chat_message_;
    std::string applied_emotion_;
    std::string applied_status_;
    std::string applied_chat_role_;
    std::string applied_chat_message_;
    std::string chrome_mode_;
    std::string chrome_state_;
    std::string chrome_action_;
    bool chrome_action_visible_ = false;
    std::string applied_chrome_mode_;
    std::string applied_chrome_state_;
    std::string applied_chrome_action_;
    bool applied_chrome_action_visible_ = false;
    bool chrome_applied_ = false;

    bool EnsureVoiceChromeObjects();
    void ApplyVoiceChrome();
    bool SetOverlayLabel(const char* name, const char* text, bool visible,
                         uint32_t color, uint32_t bg_color, bool bg_enabled);

};

} // namespace emote
