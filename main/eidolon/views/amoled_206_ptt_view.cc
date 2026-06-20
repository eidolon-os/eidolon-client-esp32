#include "eidolon/views/amoled_206_ptt_view.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display.h"

namespace eidolon {

namespace {

// Mode badge text. ASCII on purpose: the 2.06 ships a subset CJK font (basic) that
// lacks many glyphs, but full Latin renders. "PTT" / "Stream" read unambiguously.
const char* ModeBadgeText(InteractionMode mode)
{
    return mode == InteractionMode::PushToTalk ? "PTT" : "Stream";
}

const char* DefaultButtonLabel(VoiceSessionButtonState state)
{
    switch (state) {
    case VoiceSessionButtonState::Start:
        return Lang::Strings::ROOM_START;
    case VoiceSessionButtonState::Cancel:
        return Lang::Strings::ROOM_CANCEL;
    case VoiceSessionButtonState::End:
        return Lang::Strings::ROOM_END;
    case VoiceSessionButtonState::Talk:
        // The hold/release label is data-driven via snapshot.button_label; this is
        // only the fallback.
        return Lang::Strings::EIDOLON_PTT_HOLD;
    case VoiceSessionButtonState::Hidden:
    default:
        return "";
    }
}

void OnTalkButtonEvent(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
    // Two phases on one button: a tap (CLICKED) connects when not in the room;
    // once in-room, press/release drive hold-to-talk. PttPress/Release are no-ops
    // unless in-room, and ToggleVoiceSession is a no-op while already in-room, so
    // the stray CLICKED after a normal hold-release is harmless.
    if (code == LV_EVENT_PRESSED) {
        Application::GetInstance().PttPress();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        Application::GetInstance().PttRelease();
    } else if (code == LV_EVENT_CLICKED) {
        Application::GetInstance().ToggleVoiceSession();
    }
#else
    if (code == LV_EVENT_CLICKED) {
        Application::GetInstance().ToggleVoiceSession();
    }
#endif
}

}  // namespace

void Amoled206PttView::Build(const BuildContext& ctx)
{
    display_ = ctx.display;

    // Always-visible interaction-mode badge, centered just under the status bar.
    mode_badge_ = lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(mode_badge_);
    lv_obj_set_style_radius(mode_badge_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mode_badge_, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(mode_badge_, ctx.accent_color, 0);
    lv_obj_set_style_pad_hor(mode_badge_, 14, 0);
    lv_obj_set_style_pad_ver(mode_badge_, 4, 0);
    lv_obj_set_size(mode_badge_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(mode_badge_, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_remove_flag(mode_badge_, LV_OBJ_FLAG_SCROLLABLE);

    mode_badge_label_ = lv_label_create(mode_badge_);
    if (ctx.font != nullptr) {
        lv_obj_set_style_text_font(mode_badge_label_, ctx.font, 0);
    }
    lv_label_set_text(mode_badge_label_, "");
    lv_obj_center(mode_badge_label_);

    // Large hold-to-talk button in the lower-middle (not at the bottom edge).
    talk_btn_ = lv_btn_create(ctx.parent);
    lv_obj_set_width(talk_btn_, LV_HOR_RES * 84 / 100);
    lv_obj_set_height(talk_btn_, 112);
    lv_obj_align(talk_btn_, LV_ALIGN_CENTER, 0, 110);
    lv_obj_set_style_radius(talk_btn_, 18, 0);
    lv_obj_set_style_bg_color(talk_btn_, ctx.accent_color, 0);

    talk_label_ = lv_label_create(talk_btn_);
    if (ctx.font != nullptr) {
        lv_obj_set_style_text_font(talk_label_, ctx.font, 0);
    }
    lv_label_set_text(talk_label_, Lang::Strings::EIDOLON_PTT_HOLD);
    lv_obj_center(talk_label_);

    lv_obj_add_event_cb(talk_btn_, OnTalkButtonEvent, LV_EVENT_ALL, nullptr);
}

void Amoled206PttView::Render(const EidolonUiSnapshot& snapshot)
{
    if (display_ != nullptr) {
        display_->SetStatus(snapshot.status_text);
        display_->SetEmotion(snapshot.emotion);
        if (snapshot.subtitle != nullptr && snapshot.subtitle[0] != '\0') {
            display_->SetChatMessage(snapshot.subtitle_role, snapshot.subtitle);
        }
    }

    DisplayLockGuard lock(display_);
    if (mode_badge_label_ != nullptr) {
        lv_label_set_text(mode_badge_label_, ModeBadgeText(snapshot.mode));
    }

    if (talk_btn_ == nullptr || talk_label_ == nullptr) {
        return;
    }
    if (snapshot.button_state == VoiceSessionButtonState::Hidden) {
        lv_obj_add_flag(talk_btn_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(talk_btn_, LV_OBJ_FLAG_HIDDEN);
    const char* label = snapshot.button_label != nullptr ? snapshot.button_label
                                                         : DefaultButtonLabel(snapshot.button_state);
    lv_label_set_text(talk_label_, label != nullptr ? label : "");
}

void Amoled206PttView::ShowChatMessage(const char* role, const char* content)
{
    if (display_ != nullptr) {
        display_->SetChatMessage(role, content);
    }
}

}  // namespace eidolon
