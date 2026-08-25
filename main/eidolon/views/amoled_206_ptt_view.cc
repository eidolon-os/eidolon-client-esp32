#include "eidolon/views/amoled_206_ptt_view.h"

#include "display.h"

#include <esp_log.h>

namespace eidolon {

namespace {
static constexpr const char* kTag = "Amoled206Ptt";

// Mode badge text. ASCII on purpose: the 2.06 ships a subset CJK font (basic) that
// lacks many glyphs, but full Latin renders. "PTT" / "LIVE" read unambiguously.
const char* ModeBadgeText(InteractionMode mode)
{
    return mode == InteractionMode::PushToTalk ? "PTT" : "LIVE";
}

const char* RingLabelText(const EidolonUiModel& model)
{
    if (model.primary_label != nullptr && model.primary_label[0] != '\0') {
        return model.primary_label;
    }
    return model.state_label;
}

lv_color_t RingColor(const EidolonUiModel& model, lv_color_t accent_color, lv_color_t live_color,
                     lv_color_t idle_color, lv_color_t muted_color)
{
    if (model.severity == UiSeverity::Error) {
        return lv_color_hex(0xFF5B63);
    }
    if (model.severity == UiSeverity::Attention) {
        return lv_color_hex(0xF5B84B);
    }
    if (model.show_mute_icon) {
        return muted_color;
    }
    if (model.scene == UiScene::OpeningConversation ||
        model.scene == UiScene::Reconnecting) {
        return lv_color_hex(0x5EC8FF);
    }
    if (model.scene != UiScene::Conversation) {
        return idle_color;
    }

    switch (model.turn) {
    case TurnPhase::Recording:
    case TurnPhase::UserSpeaking:
        return accent_color;
    case TurnPhase::Committing:
    case TurnPhase::AgentThinking:
        return lv_color_hex(0xA78BFA);
    case TurnPhase::AgentSpeaking:
        return live_color;
    case TurnPhase::Idle:
    default:
        return model.interaction_mode == InteractionMode::PushToTalk ? idle_color : live_color;
    }
}

int RingBorderWidth(const EidolonUiModel& model)
{
    switch (model.turn) {
    case TurnPhase::Recording:
    case TurnPhase::UserSpeaking:
    case TurnPhase::AgentSpeaking:
        return 9;
    case TurnPhase::Committing:
    case TurnPhase::AgentThinking:
        return 7;
    case TurnPhase::Idle:
    default:
        return 4;
    }
}

void OnEndButtonEvent(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(kTag, "[ui] end button clicked");
        DispatchEidolonUiIntent(UiIntent::CloseConversation);
    }
}

const char* FooterText(const EidolonUiModel& model)
{
    return model.subtitle != nullptr && model.subtitle[0] != '\0' ? model.subtitle
                                                                   : model.detail_text;
}

}  // namespace

void Amoled206PttView::Build(const BuildContext& ctx)
{
    display_ = ctx.display;
    legacy_status_label_ = ctx.legacy_status_label;
    legacy_emoji_box_ = ctx.legacy_emoji_box;
    accent_color_ = ctx.accent_color;
    live_color_ = lv_color_hex(0x37D67A);
    idle_color_ = lv_color_hex(0x7B8794);
    muted_color_ = lv_color_hex(0x5A6472);
    if (legacy_status_label_ != nullptr) {
        lv_label_set_text(legacy_status_label_, "");
    }
    SetObjectVisible(legacy_status_label_, false);
    SetObjectVisible(legacy_emoji_box_, false);

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

    // Central voice ring: the main glanceable state for both PTT and full-duplex.
    ring_outer_ = lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(ring_outer_);
    lv_obj_set_size(ring_outer_, 196, 196);
    lv_obj_set_style_radius(ring_outer_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring_outer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring_outer_, 4, 0);
    lv_obj_set_style_border_color(ring_outer_, idle_color_, 0);
    lv_obj_set_style_border_opa(ring_outer_, LV_OPA_90, 0);
    lv_obj_align(ring_outer_, LV_ALIGN_CENTER, 0, -28);
    lv_obj_remove_flag(ring_outer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ring_outer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ring_outer_, Amoled206PttView::OnRingEvent, LV_EVENT_ALL, this);

    ring_inner_ = lv_obj_create(ring_outer_);
    lv_obj_remove_style_all(ring_inner_);
    lv_obj_set_size(ring_inner_, 138, 138);
    lv_obj_set_style_radius(ring_inner_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring_inner_, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(ring_inner_, idle_color_, 0);
    lv_obj_set_style_border_width(ring_inner_, 1, 0);
    lv_obj_set_style_border_color(ring_inner_, idle_color_, 0);
    lv_obj_set_style_border_opa(ring_inner_, LV_OPA_40, 0);
    lv_obj_center(ring_inner_);
    lv_obj_clear_flag(ring_inner_, LV_OBJ_FLAG_CLICKABLE);

    ring_label_ = lv_label_create(ring_outer_);
    if (ctx.font != nullptr) {
        lv_obj_set_style_text_font(ring_label_, ctx.font, 0);
    }
    lv_obj_set_style_text_color(ring_label_, lv_color_hex(0xF7FAFC), 0);
    lv_label_set_text(ring_label_, "");
    lv_obj_center(ring_label_);

    end_btn_ = lv_btn_create(ctx.parent);
    lv_obj_set_size(end_btn_, 38, 38);
    lv_obj_align(end_btn_, LV_ALIGN_TOP_RIGHT, -24, 42);
    StyleEndButton(end_btn_);
    lv_obj_add_event_cb(end_btn_, OnEndButtonEvent, LV_EVENT_CLICKED, nullptr);

    end_label_ = lv_label_create(end_btn_);
    if (ctx.font != nullptr) {
        lv_obj_set_style_text_font(end_label_, ctx.font, 0);
    }
    lv_obj_set_style_text_color(end_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(end_label_, "x");
    lv_obj_center(end_label_);
    lv_obj_add_flag(end_btn_, LV_OBJ_FLAG_HIDDEN);
}

void Amoled206PttView::Render(const EidolonUiModel& model)
{
    if (display_ != nullptr) {
        RenderFooter(model);
    }

    DisplayLockGuard lock(display_);
    last_primary_intent_ = model.primary_intent;
    last_primary_enabled_ = model.primary_enabled;
    if (legacy_status_label_ != nullptr) {
        lv_label_set_text(legacy_status_label_, "");
    }
    SetObjectVisible(legacy_status_label_, false);
    SetObjectVisible(legacy_emoji_box_, false);
    RenderModeBadge(model);
    RenderVoiceRing(model);
    RenderControls(model);
}

void Amoled206PttView::SetObjectVisible(lv_obj_t* obj, bool visible)
{
    if (obj == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void Amoled206PttView::StyleEndButton(lv_obj_t* btn)
{
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xD94B54), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0xD94B54), 0);
}

void Amoled206PttView::RenderModeBadge(const EidolonUiModel& model)
{
    if (mode_badge_label_ == nullptr || mode_badge_ == nullptr) {
        return;
    }
    lv_color_t color = model.interaction_mode == InteractionMode::PushToTalk
                           ? accent_color_
                           : live_color_;
    lv_obj_set_style_bg_color(mode_badge_, color, 0);
    lv_label_set_text(mode_badge_label_, ModeBadgeText(model.interaction_mode));
}

void Amoled206PttView::RenderVoiceRing(const EidolonUiModel& model)
{
    if (ring_outer_ == nullptr || ring_inner_ == nullptr || ring_label_ == nullptr) {
        return;
    }
    lv_color_t color = RingColor(model, accent_color_, live_color_, idle_color_, muted_color_);
    int border_width = RingBorderWidth(model);
    int inner_size = 132;
    if (model.turn == TurnPhase::Recording || model.turn == TurnPhase::UserSpeaking) {
        inner_size = 150;
    } else if (model.turn == TurnPhase::AgentSpeaking) {
        inner_size = 144;
    } else if (model.turn == TurnPhase::Committing || model.turn == TurnPhase::AgentThinking) {
        inner_size = 126;
    }

    lv_obj_set_style_border_width(ring_outer_, border_width, 0);
    lv_obj_set_style_border_color(ring_outer_, color, 0);
    lv_obj_set_style_shadow_width(ring_outer_, model.scene == UiScene::Conversation ? 24 : 10,
                                  0);
    lv_obj_set_style_shadow_opa(ring_outer_, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(ring_outer_, color, 0);

    lv_obj_set_size(ring_inner_, inner_size, inner_size);
    lv_obj_set_style_bg_color(ring_inner_, color, 0);
    lv_obj_set_style_border_color(ring_inner_, color, 0);
    lv_label_set_text(ring_label_, RingLabelText(model));
    lv_obj_center(ring_inner_);
    lv_obj_center(ring_label_);
}

void Amoled206PttView::RenderControls(const EidolonUiModel& model)
{
    SetObjectVisible(end_btn_, model.show_end_action);
}

void Amoled206PttView::RenderFooter(const EidolonUiModel& model)
{
    if (display_ == nullptr) {
        return;
    }
    display_->SetChatMessage(model.subtitle_role, FooterText(model));
}

void Amoled206PttView::OnRingEvent(lv_event_t* e)
{
    auto* view = static_cast<Amoled206PttView*>(lv_event_get_user_data(e));
    if (view != nullptr) {
        view->HandleRingEvent(e);
    }
}

void Amoled206PttView::HandleRingEvent(lv_event_t* e)
{
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && last_primary_enabled_ &&
        last_primary_intent_ == UiIntent::BeginTalk) {
        talk_gesture_active_ = true;
        DispatchEidolonUiIntent(UiIntent::BeginTalk);
        return;
    }
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
        talk_gesture_active_) {
        talk_gesture_active_ = false;
        DispatchEidolonUiIntent(UiIntent::CommitTalk);
        return;
    }
    if (code == LV_EVENT_CLICKED && last_primary_enabled_ &&
        (last_primary_intent_ == UiIntent::OpenConversation ||
         last_primary_intent_ == UiIntent::ToggleMicrophone)) {
        DispatchEidolonUiIntent(last_primary_intent_);
    }
}

}  // namespace eidolon
