#include "eidolon/views/amoled_206_ptt_view.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display.h"

namespace eidolon {

namespace {

// Mode badge text. ASCII on purpose: the 2.06 ships a subset CJK font (basic) that
// lacks many glyphs, but full Latin renders. "PTT" / "LIVE" read unambiguously.
const char* ModeBadgeText(InteractionMode mode)
{
    return mode == InteractionMode::PushToTalk ? "PTT" : "LIVE";
}

bool IsInterrupting(const EidolonUiSnapshot& snapshot)
{
    return snapshot.input_policy.barge_in_enabled &&
           snapshot.input_policy.interrupt == InterruptPhase::Interrupting;
}

const char* RingLabelText(const EidolonUiSnapshot& snapshot)
{
    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
    case ConnectionPhase::Reconnecting:
        return "...";
    case ConnectionPhase::Unreachable:
    case ConnectionPhase::Error:
        return "!";
    case ConnectionPhase::InRoom:
        break;
    case ConnectionPhase::Offline:
    case ConnectionPhase::Ready:
    default:
        return "JOIN";
    }

    if (snapshot.show_mute_icon) {
        return "MUTE";
    }
    if (IsInterrupting(snapshot)) {
        return "MIC";
    }
    switch (snapshot.turn) {
    case TurnPhase::Recording:
    case TurnPhase::UserSpeaking:
        return "REC";
    case TurnPhase::Committing:
        return "SEND";
    case TurnPhase::AgentThinking:
        return "AI";
    case TurnPhase::AgentSpeaking:
        return "AI";
    case TurnPhase::Idle:
    default:
        return "";
    }
}

lv_color_t RingColor(const EidolonUiSnapshot& snapshot, lv_color_t accent_color, lv_color_t live_color,
                     lv_color_t idle_color, lv_color_t muted_color)
{
    if (snapshot.show_mute_icon) {
        return muted_color;
    }
    if (IsInterrupting(snapshot)) {
        return accent_color;
    }
    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
    case ConnectionPhase::Reconnecting:
        return lv_color_hex(0x5EC8FF);
    case ConnectionPhase::Unreachable:
    case ConnectionPhase::Error:
        return lv_color_hex(0xFF5B63);
    case ConnectionPhase::InRoom:
        break;
    case ConnectionPhase::Offline:
    case ConnectionPhase::Ready:
    default:
        return idle_color;
    }

    switch (snapshot.turn) {
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
        return snapshot.mode == InteractionMode::PushToTalk ? idle_color : live_color;
    }
}

int RingBorderWidth(const EidolonUiSnapshot& snapshot)
{
    if (IsInterrupting(snapshot)) {
        return 9;
    }
    switch (snapshot.turn) {
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

bool CanRequestJoin(ConnectionPhase connection)
{
    return connection == ConnectionPhase::Offline ||
           connection == ConnectionPhase::Ready ||
           connection == ConnectionPhase::Unreachable ||
           connection == ConnectionPhase::Error;
}

void OnEndButtonEvent(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().RequestVoiceLeave();
        });
    }
}

const char* FooterText(const EidolonUiSnapshot& snapshot)
{
    if (snapshot.subtitle != nullptr && snapshot.subtitle[0] != '\0') {
        return snapshot.subtitle;
    }
    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
        return Lang::Strings::ROOM_CONNECTING;
    case ConnectionPhase::Reconnecting:
        return Lang::Strings::EIDOLON_RECONNECTING;
    case ConnectionPhase::Unreachable:
        return Lang::Strings::EIDOLON_SERVER_UNREACHABLE_HINT;
    case ConnectionPhase::Error:
        return Lang::Strings::ERROR;
    case ConnectionPhase::Offline:
    case ConnectionPhase::Ready:
        return Lang::Strings::EIDOLON_CONNECT;
    case ConnectionPhase::InRoom:
        break;
    }

    if (snapshot.show_mute_icon) {
        return Lang::Strings::MIC_MUTED;
    }
    if (IsInterrupting(snapshot)) {
        return Lang::Strings::LISTENING;
    }
    switch (snapshot.turn) {
    case TurnPhase::Recording:
        return Lang::Strings::EIDOLON_PTT_RELEASE;
    case TurnPhase::Committing:
    case TurnPhase::AgentThinking:
        return Lang::Strings::EIDOLON_THINKING;
    case TurnPhase::AgentSpeaking:
        return Lang::Strings::SPEAKING;
    case TurnPhase::UserSpeaking:
        return Lang::Strings::LISTENING;
    case TurnPhase::Idle:
    default:
        return snapshot.mode == InteractionMode::PushToTalk ? Lang::Strings::EIDOLON_PTT_HOLD
                                                            : Lang::Strings::LISTENING;
    }
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

void Amoled206PttView::Render(const EidolonUiSnapshot& snapshot)
{
    if (display_ != nullptr) {
        RenderFooter(snapshot);
    }

    DisplayLockGuard lock(display_);
    last_mode_ = snapshot.mode;
    last_connection_ = snapshot.connection;
    if (snapshot.connection != ConnectionPhase::Connecting &&
        snapshot.connection != ConnectionPhase::Reconnecting) {
        join_request_pending_ = false;
    }
    if (legacy_status_label_ != nullptr) {
        lv_label_set_text(legacy_status_label_, "");
    }
    SetObjectVisible(legacy_status_label_, false);
    SetObjectVisible(legacy_emoji_box_, false);
    RenderModeBadge(snapshot);
    RenderVoiceRing(snapshot);
    RenderControls(snapshot);
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

void Amoled206PttView::RenderModeBadge(const EidolonUiSnapshot& snapshot)
{
    if (mode_badge_label_ == nullptr || mode_badge_ == nullptr) {
        return;
    }
    lv_color_t color = snapshot.mode == InteractionMode::PushToTalk ? accent_color_ : live_color_;
    lv_obj_set_style_bg_color(mode_badge_, color, 0);
    lv_label_set_text(mode_badge_label_, ModeBadgeText(snapshot.mode));
}

void Amoled206PttView::RenderVoiceRing(const EidolonUiSnapshot& snapshot)
{
    if (ring_outer_ == nullptr || ring_inner_ == nullptr || ring_label_ == nullptr) {
        return;
    }
    lv_color_t color = RingColor(snapshot, accent_color_, live_color_, idle_color_, muted_color_);
    int border_width = RingBorderWidth(snapshot);
    int inner_size = 132;
    if (snapshot.turn == TurnPhase::Recording || snapshot.turn == TurnPhase::UserSpeaking) {
        inner_size = 150;
    } else if (snapshot.turn == TurnPhase::AgentSpeaking) {
        inner_size = 144;
    } else if (snapshot.turn == TurnPhase::Committing || snapshot.turn == TurnPhase::AgentThinking) {
        inner_size = 126;
    }

    lv_obj_set_style_border_width(ring_outer_, border_width, 0);
    lv_obj_set_style_border_color(ring_outer_, color, 0);
    lv_obj_set_style_shadow_width(ring_outer_, snapshot.connection == ConnectionPhase::InRoom ? 24 : 10, 0);
    lv_obj_set_style_shadow_opa(ring_outer_, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(ring_outer_, color, 0);

    lv_obj_set_size(ring_inner_, inner_size, inner_size);
    lv_obj_set_style_bg_color(ring_inner_, color, 0);
    lv_obj_set_style_border_color(ring_inner_, color, 0);
    lv_label_set_text(ring_label_, RingLabelText(snapshot));
    lv_obj_center(ring_inner_);
    lv_obj_center(ring_label_);
}

void Amoled206PttView::RenderControls(const EidolonUiSnapshot& snapshot)
{
    bool active = snapshot.connection == ConnectionPhase::InRoom ||
                  snapshot.connection == ConnectionPhase::Connecting ||
                  snapshot.connection == ConnectionPhase::Reconnecting;
    SetObjectVisible(end_btn_, active);
}

void Amoled206PttView::RenderFooter(const EidolonUiSnapshot& snapshot)
{
    if (display_ == nullptr) {
        return;
    }
    display_->SetChatMessage(snapshot.subtitle_role, FooterText(snapshot));
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
    lv_event_code_t code = lv_event_get_code(e);
    if (last_mode_ == InteractionMode::PushToTalk) {
        if (last_connection_ != ConnectionPhase::InRoom) {
            if (code == LV_EVENT_CLICKED && CanRequestJoin(last_connection_) && !join_request_pending_) {
                join_request_pending_ = true;
                Application::GetInstance().RequestVoiceJoin();
            }
            return;
        }
        if (code == LV_EVENT_PRESSED) {
            Application::GetInstance().PttPress();
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            Application::GetInstance().PttRelease();
        }
        return;
    }

    if (code != LV_EVENT_CLICKED) {
        return;
    }
    if (last_connection_ == ConnectionPhase::InRoom) {
        Application::GetInstance().ToggleMicrophone();
    } else if (CanRequestJoin(last_connection_) && !join_request_pending_) {
        join_request_pending_ = true;
        Application::GetInstance().RequestVoiceJoin();
    }
}

void Amoled206PttView::ShowChatMessage(const char* role, const char* content)
{
    if (display_ != nullptr) {
        display_->SetChatMessage(role, content);
    }
}

}  // namespace eidolon
