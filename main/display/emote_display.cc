#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>
#include <string>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"


namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";
static constexpr uint32_t kChromeSubtleText = 0xA7B2C3;
static constexpr uint32_t kChromeStateText = 0xBFFFE1;
static constexpr uint32_t kChromeStateBg = 0x102A24;
static constexpr uint32_t kChromeExitText = 0xFF6B6B;
static constexpr uint32_t kChromeExitBg = 0x2B0E14;
static constexpr uint32_t kChromeCaptionText = 0xE5E7EB;
static constexpr uint32_t kChromeCaptionBg = 0x111827;
static constexpr uint32_t kPresenceMutedText = 0x94A3B8;
static constexpr uint32_t kPresenceMutedBg = 0x172033;
static constexpr uint32_t kPresenceWarmupText = 0xFACC15;
static constexpr uint32_t kPresenceWarmupBg = 0x332A0A;
static constexpr uint32_t kPresenceActiveText = 0x86EFAC;
static constexpr uint32_t kPresenceActiveBg = 0x12351F;

static constexpr const char* kModeLabel = "eidolon_mode_label";
static constexpr const char* kStateLabel = "eidolon_state_label";
static constexpr const char* kExitLabel = "eidolon_exit_label";
static constexpr const char* kCaptionLabel = "eidolon_caption_label";
static constexpr const char* kPresenceLabel = "eidolon_presence_label";

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel == nullptr) {
        ESP_LOGE(TAG, "LCD flush skipped: panel is null");
        emote_notify_flush_finished(handle);
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        // A rejected asynchronous transaction has no completion callback. The
        // graphics task is waiting while holding its render mutex, so it must be
        // released explicitly or every later UI update blocks indefinitely.
        ESP_LOGE(TAG, "LCD flush rejected: %s", esp_err_to_name(err));
        emote_notify_flush_finished(handle);
    }
}

static const char* NormalizeEmotionName(const char* emotion)
{
    if (!emotion || std::strlen(emotion) == 0) {
        return nullptr;
    }
    if (std::strcmp(emotion, "microchip_ai") == 0) {
        return "idle";
    }
    if (std::strcmp(emotion, "cloud_slash") == 0 ||
        std::strcmp(emotion, "circle_xmark") == 0 ||
        std::strcmp(emotion, "triangle_exclamation") == 0) {
        return "sad";
    }
    if (std::strcmp(emotion, "cloud_arrow_down") == 0 ||
        std::strcmp(emotion, "download") == 0) {
        return "thinking";
    }
    if (std::strcmp(emotion, "gear") == 0 ||
        std::strcmp(emotion, "link") == 0) {
        return "neutral";
    }
    static constexpr const char* kKnownEmotes[] = {
        "angry", "confident", "confused", "crying", "delicious",
        "embarrassed", "funny", "happy", "idle", "laughing", "loving",
        "neutral", "relaxed", "sad", "shocked", "silly", "sleepy",
        "surprised", "thinking", "winking",
    };
    for (const char* known : kKnownEmotes) {
        if (std::strcmp(emotion, known) == 0) {
            return emotion;
        }
    }
    ESP_LOGW(TAG, "Unsupported emote emotion '%s', using neutral", emotion);
    return "neutral";
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    const char* normalized = NormalizeEmotionName(emotion);
    ESP_LOGI(TAG, "SetEmotion: %s -> %s", emotion ? emotion : "(null)",
             normalized ? normalized : "(ignored)");
    if (emote_handle_ && normalized) {
        pending_emotion_ = normalized;
        if (assets_loaded_ && applied_emotion_ != pending_emotion_ &&
            emote_set_anim_emoji(emote_handle_, pending_emotion_.c_str()) == ESP_OK) {
            applied_emotion_ = pending_emotion_;
        }
    }
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (!emote_handle_ || !content) {
        return;
    }
    pending_chat_role_ = role ? role : "system";
    pending_chat_message_ = content;
    if (!assets_loaded_) {
        return;
    }
    if (applied_chat_role_ == pending_chat_role_ &&
        applied_chat_message_ == pending_chat_message_) {
        return;
    }
    if (strlen(content) == 0) {
        if (SetOverlayLabel(kCaptionLabel, "", false, kChromeCaptionText,
                            kChromeCaptionBg, true)) {
            applied_chat_role_ = pending_chat_role_;
            applied_chat_message_ = pending_chat_message_;
        }
        return;
    }
    const char* safe_role = role ? role : "";
    if ((std::strcmp(safe_role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
        size_t len = strlen(content);
        char* new_content = new char[len + 1];
        strcpy(new_content, content);
        std::replace(new_content, new_content + len, static_cast<char>(0x0A), static_cast<char>(0x20));
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
        delete[] new_content;
        applied_chat_role_ = pending_chat_role_;
        applied_chat_message_ = pending_chat_message_;
        return;
    }
    if (SetOverlayLabel(kCaptionLabel, content, true, kChromeCaptionText,
                        kChromeCaptionBg, true)) {
        applied_chat_role_ = pending_chat_role_;
        applied_chat_message_ = pending_chat_message_;
    }
}

void EmoteDisplay::SetEidolonLifecycle(const char* state, const char* detail)
{
    SetVoiceChrome("EIDOLON", state ? state : "", "", false);
    SetChatMessage("system", detail ? detail : "");
}

void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        if (applied_status_ == status) {
            return;
        }
        // Eidolon owns state text and expression selection. The legacy emote
        // events also paint a microphone/speaker icon in the top-left corner,
        // which is now reserved for the radar presence badge.
        HideLegacyStatusObjects();
        applied_status_ = status;
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::SetVoiceChrome(const char* mode, const char* state, const char* action,
                                  bool action_visible)
{
    chrome_mode_ = mode ? mode : "";
    chrome_state_ = state ? state : "";
    chrome_action_ = action ? action : "";
    chrome_action_visible_ = action_visible && !chrome_action_.empty();
    ApplyVoiceChrome();
}

void EmoteDisplay::SetPresenceState(PresenceState state)
{
    if (presence_state_ == state && presence_applied_) {
        return;
    }
    presence_state_ = state;
    ApplyPresenceState();
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock()
{
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && assets_loaded_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::OnAssetsUnloaded()
{
    assets_loaded_ = false;
    applied_emotion_.clear();
    applied_status_.clear();
    applied_chat_role_.clear();
    applied_chat_message_.clear();
    applied_chrome_mode_.clear();
    applied_chrome_state_.clear();
    applied_chrome_action_.clear();
    applied_chrome_action_visible_ = false;
    chrome_applied_ = false;
    presence_applied_ = false;
}

void EmoteDisplay::OnAssetsLoaded()
{
    assets_loaded_ = true;
    if (!EnsureVoiceChromeObjects()) {
        ESP_LOGE(TAG, "Failed to create Eidolon voice chrome objects");
    }
    HideLegacyStatusObjects();
    applied_emotion_.clear();
    applied_status_.clear();
    applied_chat_role_.clear();
    applied_chat_message_.clear();
    chrome_applied_ = false;
    if (emote_handle_ && !pending_emotion_.empty() &&
        emote_set_anim_emoji(emote_handle_, pending_emotion_.c_str()) == ESP_OK) {
        applied_emotion_ = pending_emotion_;
    }
    ApplyVoiceChrome();
    ApplyPresenceState();
    SetChatMessage(pending_chat_role_.c_str(), pending_chat_message_.c_str());
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

bool EmoteDisplay::EnsureVoiceChromeObjects()
{
    if (!emote_handle_) {
        return false;
    }

    struct LabelLayout {
        const char* name;
        uint8_t align;
        int16_t x;
        int16_t y;
        uint16_t width;
        uint16_t height;
        gfx_text_align_t text_align;
        gfx_label_long_mode_t long_mode;
        int scroll_speed;
    };
    static constexpr LabelLayout kLayouts[] = {
        {kPresenceLabel, GFX_ALIGN_TOP_LEFT, 8, 10, 98, 26,
         GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP, 0},
        {kModeLabel, GFX_ALIGN_TOP_MID, 0, 8, 92, 22,
         GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP, 0},
        {kExitLabel, GFX_ALIGN_TOP_RIGHT, -8, 8, 58, 28,
         GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP, 0},
        {kStateLabel, GFX_ALIGN_TOP_MID, 0, 34, 100, 28,
         GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP, 0},
        {kCaptionLabel, GFX_ALIGN_BOTTOM_MID, 0, -4, 300, 26,
         GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP, 0},
    };

    bool success = true;
    for (const auto& layout : kLayouts) {
        gfx_obj_t* obj = emote_get_obj_by_name(emote_handle_, layout.name);
        if (!obj) {
            // Create after emote_load_assets(): the animation objects already
            // exist, so these labels remain above the expression scene.
            obj = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL,
                                           layout.name);
        }
        if (!obj) {
            ESP_LOGE(TAG, "Failed to create overlay label: %s", layout.name);
            success = false;
            continue;
        }

        emote_lock(emote_handle_);
        gfx_obj_align(obj, layout.align, layout.x, layout.y);
        gfx_obj_set_size(obj, layout.width, layout.height);
        gfx_label_set_text_align(obj, layout.text_align);
        gfx_label_set_long_mode(obj, layout.long_mode);
        if (layout.long_mode == GFX_LABEL_LONG_SCROLL) {
            gfx_label_set_scroll_speed(obj, layout.scroll_speed);
            gfx_label_set_scroll_loop(obj, true);
        }
        gfx_obj_set_visible(obj, false);
        emote_unlock(emote_handle_);
    }
    return success;
}

void EmoteDisplay::ApplyVoiceChrome()
{
    if (!emote_handle_ || !assets_loaded_) {
        return;
    }
    bool success = true;
    if (!chrome_applied_ || applied_chrome_mode_ != chrome_mode_) {
        success = SetOverlayLabel(kModeLabel, chrome_mode_.c_str(), !chrome_mode_.empty(),
                                  kChromeSubtleText, 0, false) && success;
    }
    if (!chrome_applied_ || applied_chrome_state_ != chrome_state_) {
        success = SetOverlayLabel(kStateLabel, chrome_state_.c_str(), !chrome_state_.empty(),
                                  kChromeStateText, kChromeStateBg, true) && success;
    }
    if (!chrome_applied_ || applied_chrome_action_ != chrome_action_ ||
        applied_chrome_action_visible_ != chrome_action_visible_) {
        success = SetOverlayLabel(kExitLabel, chrome_action_.c_str(), chrome_action_visible_,
                                  kChromeExitText, kChromeExitBg, true) && success;
    }
    if (success) {
        applied_chrome_mode_ = chrome_mode_;
        applied_chrome_state_ = chrome_state_;
        applied_chrome_action_ = chrome_action_;
        applied_chrome_action_visible_ = chrome_action_visible_;
        chrome_applied_ = true;
    }
}

void EmoteDisplay::ApplyPresenceState()
{
    if (!emote_handle_ || !assets_loaded_ ||
        (presence_applied_ && applied_presence_state_ == presence_state_)) {
        return;
    }

    const char* text = "RADAR --";
    uint32_t text_color = kPresenceMutedText;
    uint32_t bg_color = kPresenceMutedBg;
    switch (presence_state_) {
    case PresenceState::Calibrating:
        text = "RADAR ...";
        text_color = kPresenceWarmupText;
        bg_color = kPresenceWarmupBg;
        break;
    case PresenceState::Vacant:
        text = "VACANT";
        break;
    case PresenceState::Present:
        text = "PRESENT";
        text_color = kPresenceActiveText;
        bg_color = kPresenceActiveBg;
        break;
    case PresenceState::Unavailable:
    default:
        break;
    }
    if (SetOverlayLabel(kPresenceLabel, text, true, text_color, bg_color, true)) {
        applied_presence_state_ = presence_state_;
        presence_applied_ = true;
    }
}

void EmoteDisplay::HideLegacyStatusObjects()
{
    if (!emote_handle_ || !assets_loaded_) {
        return;
    }
    static constexpr const char* kLegacyObjects[] = {
        "status_icon",
        "charge_icon",
        "battery_label",
        "clock_label",
        "listen_anim",
        "toast_label",
    };
    if (emote_lock(emote_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock legacy status objects");
        return;
    }
    for (const char* name : kLegacyObjects) {
        if (gfx_obj_t* obj = emote_get_obj_by_name(emote_handle_, name)) {
            gfx_obj_set_visible(obj, false);
        }
    }
    emote_unlock(emote_handle_);
}

bool EmoteDisplay::SetOverlayLabel(const char* name, const char* text, bool visible,
                                   uint32_t color, uint32_t bg_color, bool bg_enabled)
{
    if (!emote_handle_ || !assets_loaded_ || !name) {
        return false;
    }
    gfx_obj_t* obj = emote_get_obj_by_name(emote_handle_, name);
    if (!obj) {
        ESP_LOGD(TAG, "Overlay label not found: %s", name);
        return false;
    }
    if (emote_lock(emote_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock overlay label: %s", name);
        return false;
    }
    gfx_label_set_text(obj, text ? text : "");
    gfx_label_set_color(obj, GFX_COLOR_HEX(color));
    gfx_label_set_bg_enable(obj, bg_enabled);
    if (bg_enabled) {
        gfx_label_set_bg_color(obj, GFX_COLOR_HEX(bg_color));
        gfx_label_set_opa(obj, 220);
    }
    gfx_obj_set_visible(obj, visible);
    emote_unlock(emote_handle_);
    return true;
}

} // namespace emote
