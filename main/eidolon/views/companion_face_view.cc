#include "companion_face_view.h"
#include "companion_layout.h"
#include <algorithm>
#include <cstring>
#include <esp_timer.h>
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
#include <esp_heap_caps.h>
#include <esp_log.h>
#endif
#include "display.h"
#include "eidolon/output_policy.h"
#include "eidolon/avatar/skins/default/default.h"
namespace eidolon {
namespace {
uint64_t NowMs() { return esp_timer_get_time() / 1000; }
void Visible(lv_obj_t* obj, bool visible) {
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, lv_align_t align, int x, int y) {
    auto* label=lv_label_create(parent);
    lv_obj_set_style_text_font(label,font,0);
    lv_obj_set_style_text_color(label,lv_color_white(),0);
    lv_label_set_text(label,"");
    lv_obj_align(label,align,x,y);
    return label;
}
}
CompanionFaceView::CompanionFaceView() = default;
CompanionFaceView::~CompanionFaceView() = default;
void CompanionFaceView::Build(const BuildContext& ctx) {
    display_=ctx.display;
    lv_obj_update_layout(ctx.parent);
    const companion::Layout layout(lv_obj_get_width(ctx.parent),lv_obj_get_height(ctx.parent));
    lv_obj_set_style_bg_color(ctx.parent,lv_color_black(),0);
    lv_obj_remove_flag(ctx.parent,LV_OBJ_FLAG_SCROLLABLE);
    auto panel=[&](companion::Rect r) {
        auto* obj=lv_obj_create(ctx.parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_pos(obj,r.x,r.y);lv_obj_set_size(obj,r.w,r.h);
        lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    };
    viewport_=panel(layout.face);
    avatar_=std::make_unique<stackchan::avatar::DefaultAvatar>();
    avatar_->init(viewport_,ctx.font,false);
    state_dot_=panel({12,14,6,6});
    lv_obj_set_style_radius(state_dot_,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_opa(state_dot_,LV_OPA_COVER,0);
    status_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,26,4);
    lv_obj_set_size(status_,layout.header.w-136,layout.header.h);
    lv_label_set_long_mode(status_,LV_LABEL_LONG_DOT);
    auto indicator=[&](int x) {
        auto* item=Label(ctx.parent,ctx.icon_font,LV_ALIGN_TOP_LEFT,x,4);
        lv_obj_set_style_text_color(item,lv_color_hex(0x71808D),0);
        lv_obj_set_size(item,24,28);
        lv_label_set_long_mode(item,LV_LABEL_LONG_CLIP);
        return item;
    };
    const auto width=lv_obj_get_width(ctx.parent);
    indicators_={indicator(width-84),indicator(width-60),indicator(width-36)};
    setup_=indicator(width-108);
    lv_label_set_text(setup_,LV_SYMBOL_SETTINGS);
    lv_obj_add_flag(setup_,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(setup_,OnSetup,LV_EVENT_CLICKED,this);
    Visible(setup_,false);
    detail_panel_=panel(layout.detail);
    lv_obj_add_flag(detail_panel_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail_panel_,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_panel_,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(detail_panel_,3,LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(detail_panel_,lv_color_hex(0x71808D),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(detail_panel_,LV_OPA_60,LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(detail_panel_,2,LV_PART_SCROLLBAR);
    detail_=Label(detail_panel_,ctx.font,LV_ALIGN_TOP_LEFT,0,0);
    lv_obj_set_width(detail_,layout.detail.w-8);
    lv_label_set_long_mode(detail_,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(detail_,5,0);
    information_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,layout.information.x,layout.information.y);
    lv_obj_set_size(information_,layout.information.w,layout.information.h);
    lv_obj_set_style_text_color(information_,lv_color_hex(0x9AA9B5),0);
    lv_label_set_long_mode(information_,LV_LABEL_LONG_SCROLL);
    // Use LVGL's bounded back-and-forth animation, with reading pauses.
    auto& scroll=scroll_animation_;lv_anim_init(&scroll);
    lv_anim_set_delay(&scroll,1500);lv_anim_set_repeat_delay(&scroll,2500);
    lv_anim_set_playback_delay(&scroll,1500);lv_anim_set_duration(&scroll,6000);
    lv_obj_set_style_anim(information_,&scroll,0);
    input_hint_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,12,layout.actions.y+6);
    lv_obj_set_size(input_hint_,layout.actions.w,28);
    lv_label_set_long_mode(input_hint_,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(input_hint_,lv_color_hex(0x9AA9B5),0);
    Visible(input_hint_,false);
    mode_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,12,layout.actions.y+6);
    lv_obj_set_size(mode_,72,28);
    lv_obj_set_style_text_color(mode_,lv_color_hex(0x71808D),0);
    lv_label_set_long_mode(mode_,LV_LABEL_LONG_DOT);
    auto button=[&](int x,int width) {
        auto* obj=lv_button_create(ctx.parent);
        lv_obj_set_pos(obj,x,layout.actions.y);lv_obj_set_size(obj,width,layout.actions.h);
        lv_obj_set_style_radius(obj,12,0);
        lv_obj_set_style_shadow_width(obj,0,0);
        lv_obj_set_style_border_width(obj,0,0);
        lv_obj_set_style_pad_all(obj,0,0);
        lv_obj_set_style_bg_color(obj,lv_color_hex(0x172A30),0);
        lv_obj_set_style_bg_color(obj,lv_color_hex(0x28515A),LV_STATE_PRESSED);
        return obj;
    };
    primary_=button(88,layout.actions.w-148);
    primary_label_=Label(primary_,ctx.font,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_text_color(primary_label_,lv_color_hex(0xA3E5DA),0);
    lv_obj_add_event_cb(primary_,OnPrimary,LV_EVENT_ALL,this);
    close_=button(layout.actions.w-44,56);
    lv_obj_set_style_bg_color(close_,lv_color_hex(0x242A30),0);
    auto* close_label=Label(close_,ctx.font,LV_ALIGN_CENTER,0,0);
    lv_label_set_text(close_label,"END");
    lv_obj_add_event_cb(close_,OnClose,LV_EVENT_CLICKED,this);
    Visible(primary_,false);Visible(close_,false);Visible(detail_panel_,false);
}
void CompanionFaceView::SetSystemStatus(const char* text) {
    if (!display_ || !status_) return;
    DisplayLockGuard lock(display_);
    // Once projection starts, it is the sole lifecycle state owner.
    if (status_ && !has_model_) lv_label_set_text(status_,text ? text : "");
}
void CompanionFaceView::ShowNotification(const char* text,int duration_ms) {
    if (!display_ || !information_) return;
    DisplayLockGuard lock(display_);
    notification_until_=NowMs()+std::clamp(duration_ms,1,15000);
    lv_label_set_text(information_,text ? text : "");
    UpdateLayout();
}
void CompanionFaceView::RefreshInformation() {
    if (notification_until_ && NowMs()<notification_until_) { UpdateLayout();return; }
    notification_until_=0;
    if (std::strcmp(lv_label_get_text(information_),information_text_.c_str()))
        lv_label_set_text(information_,information_text_.c_str());
    UpdateLayout();
}
void CompanionFaceView::UpdateLayout() {
    if (!viewport_) return;
    auto* parent=lv_obj_get_parent(viewport_);
    const bool information=lv_label_get_text(information_)[0]!='\0';
    const companion::Layout layout(lv_obj_get_width(parent),lv_obj_get_height(parent),has_actions_,information);
    lv_obj_set_pos(viewport_,layout.face.x,layout.face.y);
    lv_obj_set_size(viewport_,layout.face.w,layout.face.h);
    lv_obj_set_pos(information_,layout.information.x,layout.information.y);
    Visible(information_,information);
}
void CompanionFaceView::Apply(const expression::FacePose& p) {
    auto eye=[&](auto& feature,float open,float tilt) {
        feature.setPosition({static_cast<int>(p.gaze_x*100),static_cast<int>(p.gaze_y*100)});
        feature.setSize(static_cast<int>(p.eye_size*100));
        feature.setWeight(static_cast<int>(open*100));
        feature.setRotation(static_cast<int>(tilt*10));
    };
    eye(avatar_->leftEye(),p.left_open,p.left_tilt);
    eye(avatar_->rightEye(),p.right_open,p.right_tilt);
    avatar_->mouth().setWeight(static_cast<int>(p.mouth_open*100));
    avatar_->update();
}
void CompanionFaceView::Advance(uint32_t completed_frame) {
    if (!avatar_) return;
    RefreshInformation();
    if (!touch_navigation_ && lv_obj_is_visible(detail_panel_)) {
        lv_obj_update_layout(detail_panel_);
        const int extent = lv_obj_get_scroll_bottom(detail_panel_) + lv_obj_get_scroll_y(detail_panel_);
        if (extent > 0) {
            // Pause at both ends; a non-touch device must expose the entire message.
            const uint64_t travel = static_cast<uint64_t>(extent) * 50;
            const uint64_t elapsed = (NowMs() - detail_scroll_start_) % (travel + 4000);
            const int offset = elapsed < 2000 ? 0 :
                static_cast<int>(std::min<uint64_t>(extent, (elapsed - 2000) / 50));
            lv_obj_scroll_to_y(detail_panel_, offset, LV_ANIM_OFF);
        }
    }
    const bool visible=lv_obj_is_visible(avatar_->getPanel()->get());
    Emit(runtime_.SetVisible(visible));
    if (!visible) { awaiting_frame_=false;return; }
    if (awaiting_frame_) {
        if (completed_frame != prepared_frame_) return;
        awaiting_frame_=false;
        Emit(runtime_.Presented());
    }
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
    Benchmark(NowMs());
#endif
    Apply(runtime_.Tick(NowMs()));
    if (!runtime_.active()) return;
    ++prepared_frame_;
    // Invalidate even an unchanged pose so a receipt has a real flush boundary.
    lv_area_t area;
    lv_obj_get_coords(avatar_->getPanel()->get(),&area);
    // The panel is the reserved face viewport, not the whole screen.
    lv_obj_invalidate_area(avatar_->getPanel()->get(),&area);
    awaiting_frame_=true;
}
void CompanionFaceView::Emit(expression::Event event) {
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
    if (event.token >= 1000000 && event.token < 1000064) {
        if (event.status==expression::Status::Completed) ++bench_completed_;
        else if (event.status==expression::Status::Failed || event.status==expression::Status::Cancelled || event.status==expression::Status::Rejected) ++bench_failed_;
    }
#endif
    if (event.status!=expression::Status::None && observer_) observer_(event);
}
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
void CompanionFaceView::Benchmark(uint64_t now) {
    if (bench_done_ || now<20000 || runtime_.active()) return;
    if (bench_submitted_==0 || bench_submitted_==8 || bench_submitted_==64) {
        ESP_LOGI("expression-bench","submitted=%u completed=%u failed=%u internal=%u largest=%u psram=%u integrity=%d",
            bench_submitted_,bench_completed_,bench_failed_,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),heap_caps_check_integrity_all(true));
    }
    if (bench_submitted_==64) { bench_done_=true;return; }
    expression::Plan plan;plan.token=1000000+bench_submitted_;plan.count=1;plan.max_duration_ms=300;
    plan.steps[0]={static_cast<expression::Gesture>(bench_submitted_%8),.6f,0,300,false};
    ++bench_submitted_;Emit(runtime_.Submit(plan,now));
}
#endif
void CompanionFaceView::SetObserver(Observer observer) {
    DisplayLockGuard lock(display_);observer_=std::move(observer);
}
expression::Event CompanionFaceView::Submit(const expression::Plan& plan) {
    DisplayLockGuard lock(display_);
    auto event=runtime_.Submit(plan,NowMs());Emit(event);return event;
}
expression::Event CompanionFaceView::Cancel(uint64_t token) {
    DisplayLockGuard lock(display_);
    auto event=runtime_.Cancel(token);Emit(event);return event;
}
void CompanionFaceView::PulseEmotion(const char* emotion,int ttl_ms) {
    if (!emotion || ttl_ms<150 || !CurrentOutputGate().Allows(presentation::Output::Expression)) return;
    expression::Gesture gesture;
    if (!std::strcmp(emotion,"happy")) gesture=expression::Gesture::Delight;
    else if (!std::strcmp(emotion,"thinking")) gesture=expression::Gesture::Ponder;
    else if (!std::strcmp(emotion,"sad")) gesture=expression::Gesture::Soften;
    else return;
    DisplayLockGuard lock(display_);
    // Legacy local reflex uses the same recipes, never a second renderer.
    expression::Plan plan;plan.token=next_local_token_++;
    plan.max_duration_ms=std::min(ttl_ms,10000);plan.count=1;
    plan.steps[0]={gesture,.3f,0,plan.max_duration_ms,true};
    Emit(runtime_.Submit(plan,NowMs()));
}
void CompanionFaceView::Render(const EidolonUiModel& model) {
    if (!avatar_) return;
    DisplayLockGuard lock(display_);
    expression::BaseState base=expression::BaseState::Idle;
    if (model.severity==UiSeverity::Error) base=expression::BaseState::Error;
    else if (model.turn==TurnPhase::AgentThinking || model.turn==TurnPhase::Committing)
        base=expression::BaseState::Thinking;
    else if (model.turn==TurnPhase::UserSpeaking || model.turn==TurnPhase::Recording)
        base=expression::BaseState::Listening;
    runtime_.SetBase(base);
    has_model_=true;
    const auto color=model.severity==UiSeverity::Error ? 0xF58D8D :
        model.severity==UiSeverity::Attention ? 0xE5BE78 : 0x91D8CA;
    lv_obj_set_style_bg_color(state_dot_,lv_color_hex(color),0);
    lv_label_set_text(status_,model.status_text ? model.status_text : "");
    lv_label_set_text(mode_,model.mode_label ? model.mode_label : "");
    const bool touch=model.primary_presentation==UiActionPresentation::TouchControl;
    const bool hint=model.primary_presentation==UiActionPresentation::InputHint;
    primary_intent_=touch && model.primary_enabled ? model.primary_intent : UiIntent::None;
    has_actions_=primary_intent_!=UiIntent::None || model.show_end_action || hint;
    lv_label_set_text(input_hint_,hint ? model.input_hint : "");
    Visible(input_hint_,hint);
    Visible(mode_,touch || model.show_end_action);
    lv_label_set_text(primary_label_,model.primary_label ? model.primary_label : "");
    Visible(primary_,primary_intent_!=UiIntent::None);
    Visible(close_,model.show_end_action);
    Visible(setup_,model.show_setup_action);
    const bool face=model.scene==UiScene::Ready || model.scene==UiScene::Conversation ||
        model.scene==UiScene::OpeningConversation || model.scene==UiScene::Ended;
    Visible(viewport_,face);Visible(detail_panel_,!face);
    touch_navigation_=model.touch_navigation;
    const char* detail=model.detail_text ? model.detail_text : "";
    if (!face && std::strcmp(lv_label_get_text(detail_),detail)) {
        lv_label_set_text(detail_,detail);
        detail_scroll_start_=NowMs();
        lv_obj_scroll_to_y(detail_panel_,0,LV_ANIM_OFF);
    }
    // Dialogue only ever enters this dedicated strip. System instructions live
    // in a separate, manually scrollable page, never in the avatar's bubble.
    information_text_=face && model.scene!=UiScene::Conversation ? detail : "";
    if (model.scene==UiScene::Conversation &&
        CurrentOutputGate().Allows(presentation::Output::DialogueText) && model.subtitle)
        information_text_=model.subtitle;
    else if (!face) {
        lv_obj_update_layout(detail_panel_);
        information_text_=lv_obj_get_height(detail_)>lv_obj_get_height(detail_panel_) ? (touch_navigation_ ? "Swipe to read more" : "") : "";
    }
    RefreshInformation();
}
void CompanionFaceView::OnPrimary(lv_event_t* event) {
    auto* self=static_cast<CompanionFaceView*>(lv_event_get_user_data(event));
    const auto code=lv_event_get_code(event);
    if (code==LV_EVENT_PRESSED && self->primary_intent_==UiIntent::BeginTalk) {
        self->talk_gesture_active_=true;
        DispatchEidolonUiIntent(UiIntent::BeginTalk);
    } else if ((code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) && self->talk_gesture_active_) {
        self->talk_gesture_active_=false;
        DispatchEidolonUiIntent(UiIntent::CommitTalk);
    } else if (code==LV_EVENT_CLICKED &&
        (self->primary_intent_==UiIntent::OpenConversation || self->primary_intent_==UiIntent::ToggleMicrophone)) {
        DispatchEidolonUiIntent(self->primary_intent_);
    }
}
void CompanionFaceView::OnSetup(lv_event_t*) { DispatchEidolonUiIntent(UiIntent::OpenSetup); }
void CompanionFaceView::OnClose(lv_event_t*) { DispatchEidolonUiIntent(UiIntent::CloseConversation); }
}
