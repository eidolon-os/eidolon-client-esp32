#include "companion_face_view.h"
#include "companion_layout.h"
#include "companion_palette.h"
#include <algorithm>
#include <cstring>
#include <esp_timer.h>
#include <font_awesome.h>
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
#include <esp_heap_caps.h>
#include <esp_log.h>
#endif
#include "display.h"
#include "eidolon/output_policy.h"
#include "eidolon/avatar/skins/default/default.h"
#if CONFIG_EIDOLON_PET_FACE
#include <cmath>
#include <cstdio>
#include <iterator>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "board.h"
#include "eidolon/livekit_board.h"
#include "eidolon/pet/pet_sense.h"
#include "eidolon/pet/pet_chirp_player.h"
LV_FONT_DECLARE(font_awesome_30_4);
#endif
namespace eidolon {
namespace {
namespace palette = companion::palette;
uint64_t NowMs() { return esp_timer_get_time() / 1000; }
void Visible(lv_obj_t* obj, bool visible) {
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void Text(lv_obj_t* label, const char* text) {
    const char* value = text ? text : "";
    if (std::strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, lv_align_t align, int x, int y) {
    auto* label=lv_label_create(parent);
    lv_obj_set_style_text_font(label,font,0);
    lv_obj_set_style_text_color(label,lv_color_hex(palette::Ink),0);
    lv_label_set_text(label,"");
    lv_obj_align(label,align,x,y);
    return label;
}
}
CompanionFaceView::CompanionFaceView() = default;
CompanionFaceView::~CompanionFaceView() = default;
void CompanionFaceView::Build(const BuildContext& ctx) {
    display_=ctx.display;
    font_=ctx.font;
    lv_obj_update_layout(ctx.parent);
    const companion::Layout layout(lv_obj_get_width(ctx.parent),lv_obj_get_height(ctx.parent));
    lv_obj_set_style_bg_color(ctx.parent,lv_color_hex(palette::Canvas),0);
    lv_obj_remove_flag(ctx.parent,LV_OBJ_FLAG_SCROLLABLE);
    auto panel=[&](companion::Rect r) {
        auto* obj=lv_obj_create(ctx.parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_pos(obj,r.x,r.y);lv_obj_set_size(obj,r.w,r.h);
        lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    };
#if CONFIG_EIDOLON_PET_FACE
    BuildPet(ctx);   // full-screen, first child: every other widget draws above it
    viewport_=panel(layout.face);
    lv_obj_remove_flag(viewport_,LV_OBJ_FLAG_CLICKABLE);   // touches reach the face
#else
    viewport_=panel(layout.face);
    avatar_=std::make_unique<stackchan::avatar::DefaultAvatar>();
    avatar_->primaryColor=lv_color_hex(palette::Expression);
    avatar_->secondaryColor=lv_color_hex(palette::Stage);
    avatar_->init(viewport_,ctx.font,false);
    auto* stage=avatar_->getPanel()->get();
    lv_obj_set_style_radius(stage,22,0);
    lv_obj_set_style_border_width(stage,1,0);
    lv_obj_set_style_border_color(stage,lv_color_hex(palette::StageEdge),0);
#endif
    // Features sit well inside the rounded background; no full-panel clipping
    // layer or shadow is needed for this small display.
    state_dot_=panel({12,14,6,6});
    lv_obj_set_style_radius(state_dot_,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_opa(state_dot_,LV_OPA_COVER,0);
    status_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,26,4);
    lv_obj_set_size(status_,layout.header.w-112,layout.header.h);
    lv_label_set_long_mode(status_,LV_LABEL_LONG_DOT);
    auto indicator=[&](int x) {
        auto* item=Label(ctx.parent,ctx.icon_font,LV_ALIGN_TOP_LEFT,x,4);
        lv_obj_set_style_text_color(item,lv_color_hex(palette::Muted),0);
        lv_obj_set_size(item,28,28);
        lv_obj_set_style_text_align(item,LV_TEXT_ALIGN_CENTER,0);
        lv_label_set_long_mode(item,LV_LABEL_LONG_CLIP);
        return item;
    };
    const auto width=lv_obj_get_width(ctx.parent);
    indicators_={indicator(width-96),indicator(width-68),indicator(width-40)};
    // Pack only populated indicators; BOX-3 has no battery telemetry.
    lv_obj_set_style_text_color(indicators_.microphone,lv_color_hex(palette::Attention),0);
    setup_=indicator(width-124);
    lv_label_set_text(setup_,FONT_AWESOME_GEAR);
    lv_obj_set_ext_click_area(setup_,2);
    lv_obj_add_flag(setup_,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(setup_,OnSetup,LV_EVENT_CLICKED,this);
    Visible(setup_,false);
    detail_panel_=panel(layout.detail);
    lv_obj_add_flag(detail_panel_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail_panel_,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_panel_,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(detail_panel_,3,LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(detail_panel_,lv_color_hex(palette::Muted),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(detail_panel_,LV_OPA_60,LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(detail_panel_,2,LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(detail_panel_,lv_color_hex(palette::Paper),0);
    lv_obj_set_style_bg_opa(detail_panel_,LV_OPA_COVER,0);
    lv_obj_set_style_radius(detail_panel_,12,0);
    lv_obj_set_style_pad_all(detail_panel_,8,0);
    detail_=Label(detail_panel_,ctx.font,LV_ALIGN_TOP_LEFT,0,0);
    lv_obj_set_width(detail_,layout.detail.w-20);
    lv_label_set_long_mode(detail_,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(detail_,5,0);
    caption_card_=panel({4,layout.information.y-4,lv_obj_get_width(ctx.parent)-8,layout.information.h+8});
    lv_obj_set_style_bg_color(caption_card_,lv_color_hex(palette::Paper),0);
    lv_obj_set_style_bg_opa(caption_card_,LV_OPA_COVER,0);
    lv_obj_set_style_radius(caption_card_,12,0);
    Visible(caption_card_,false);
    information_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,layout.information.x,layout.information.y);
    lv_obj_set_size(information_,layout.information.w,layout.information.h);
    lv_obj_set_style_text_color(information_,lv_color_hex(palette::Muted),0);
    lv_label_set_long_mode(information_,LV_LABEL_LONG_WRAP);
    subtitle_panel_=panel(layout.information);
    lv_obj_set_scrollbar_mode(subtitle_panel_,LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(subtitle_panel_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(subtitle_panel_,LV_DIR_NONE);
    subtitle_=Label(subtitle_panel_,ctx.font,LV_ALIGN_TOP_LEFT,0,0);
    lv_obj_set_width(subtitle_,layout.information.w);
    lv_obj_set_style_text_align(subtitle_,LV_TEXT_ALIGN_LEFT,0);
    lv_label_set_long_mode(subtitle_,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(subtitle_,4,0);
    lv_obj_set_style_text_color(subtitle_,lv_color_hex(palette::Ink),0);
    Visible(subtitle_panel_,false);
    notification_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,12,36);
    lv_obj_set_size(notification_,layout.header.w,28);
    lv_label_set_long_mode(notification_,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(notification_,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_bg_color(notification_,lv_color_hex(palette::Accent),0);
    lv_obj_set_style_bg_opa(notification_,LV_OPA_COVER,0);
    lv_obj_set_style_radius(notification_,8,0);
    lv_obj_set_style_text_color(notification_,lv_color_hex(palette::Paper),0);
    Visible(notification_,false);
    // A physical-key hint is an instruction, not a control: plain text in the
    // action row with only the key outlined, so nothing on it invites a touch.
    input_hint_=lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(input_hint_);
    lv_obj_remove_flag(input_hint_,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(input_hint_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(input_hint_,layout.actions.x,layout.actions.y+2);
    lv_obj_set_size(input_hint_,layout.actions.w,28);
    lv_obj_set_flex_flow(input_hint_,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_hint_,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_hint_,6,0);
    auto hint_part=[&]() {
        auto* label=lv_label_create(input_hint_);
        lv_obj_set_style_text_font(label,ctx.font,0);
        lv_obj_set_style_text_color(label,lv_color_hex(palette::Muted),0);
        lv_label_set_text(label,"");
        return label;
    };
    hint_action_=hint_part();
    hint_key_=hint_part();
    hint_effect_=hint_part();
    lv_obj_set_style_text_color(hint_key_,lv_color_hex(palette::Ink),0);
    lv_obj_set_style_border_width(hint_key_,1,0);
    lv_obj_set_style_border_color(hint_key_,lv_color_hex(palette::Muted),0);
    lv_obj_set_style_radius(hint_key_,4,0);
    lv_obj_set_style_pad_hor(hint_key_,5,0);
    Visible(input_hint_,false);
    mode_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,12,layout.actions.y+6);
    lv_obj_set_size(mode_,72,28);
    lv_obj_set_style_text_color(mode_,lv_color_hex(palette::Muted),0);
    lv_label_set_long_mode(mode_,LV_LABEL_LONG_DOT);
    auto button=[&](int x,int width) {
        auto* obj=lv_button_create(ctx.parent);
        lv_obj_set_pos(obj,x,layout.actions.y);lv_obj_set_size(obj,width,layout.actions.h);
        lv_obj_set_style_radius(obj,12,0);
        lv_obj_set_style_shadow_width(obj,0,0);
        lv_obj_set_style_border_width(obj,0,0);
        lv_obj_set_style_pad_all(obj,0,0);
        lv_obj_set_style_bg_color(obj,lv_color_hex(palette::Accent),0);
        lv_obj_set_style_bg_color(obj,lv_color_hex(palette::Pressed),LV_STATE_PRESSED);
        return obj;
    };
    primary_=button(88,layout.actions.w-148);
    primary_label_=Label(primary_,ctx.font,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_text_color(primary_label_,lv_color_hex(palette::Paper),0);
    lv_obj_add_event_cb(primary_,OnPrimary,LV_EVENT_ALL,this);
    close_=button(layout.actions.w-44,56);
    lv_obj_set_style_bg_color(close_,lv_color_hex(palette::ErrorSoft),0);
    auto* close_label=Label(close_,ctx.font,LV_ALIGN_CENTER,0,0);
    lv_label_set_text(close_label,"END");
    lv_obj_set_style_text_color(close_label,lv_color_hex(palette::Error),0);
    lv_obj_add_event_cb(close_,OnClose,LV_EVENT_CLICKED,this);
    Visible(primary_,false);Visible(close_,false);Visible(detail_panel_,false);
#if CONFIG_EIDOLON_PET_FACE
    // Pet status bar: state only. Nothing at the left (the antenna tips carry the
    // session state; words appear there only for problems); battery, network and a
    // red microphone when muted at the right. None of it is a button.
    Visible(state_dot_,false);
    lv_obj_align(status_,LV_ALIGN_TOP_LEFT,12,4);
    lv_obj_set_style_text_color(indicators_.microphone,lv_color_hex(palette::Error),0);
    const int32_t screen_h=lv_obj_get_height(ctx.parent);
    // The whole bar is one target: tap it, or drag down from it, for the controls.
    bar_hit_=lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(bar_hit_);
    lv_obj_set_pos(bar_hit_,0,0);
    lv_obj_set_size(bar_hit_,width,36);
    lv_obj_add_flag(bar_hit_,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar_hit_,LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(bar_hit_,LV_OBJ_FLAG_SCROLLABLE);
    for (auto code : {LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_CLICKED}) lv_obj_add_event_cb(bar_hit_,OnBar,code,this);
    handle_=lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(handle_);
    lv_obj_set_size(handle_,36,4);
    lv_obj_set_pos(handle_,(width-36)/2,3);
    lv_obj_set_style_radius(handle_,2,0);
    lv_obj_set_style_bg_opa(handle_,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(handle_,lv_color_hex(palette::Muted),0);
    lv_obj_remove_flag(handle_,LV_OBJ_FLAG_CLICKABLE);
    // The panel slides down over a dimmed face; a tap on the dimmed face closes it.
    scrim_=lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(scrim_);
    lv_obj_set_size(scrim_,width,screen_h);
    lv_obj_set_style_bg_color(scrim_,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(scrim_,LV_OPA_50,0);
    lv_obj_add_flag(scrim_,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim_,OnScrim,LV_EVENT_CLICKED,this);
    panel_=lv_obj_create(ctx.parent);
    lv_obj_remove_style_all(panel_);
    lv_obj_set_size(panel_,width-16,104);
    lv_obj_set_pos(panel_,8,0);
    lv_obj_set_style_bg_color(panel_,lv_color_hex(palette::Paper),0);
    lv_obj_set_style_bg_opa(panel_,LV_OPA_COVER,0);
    lv_obj_set_style_radius(panel_,18,0);
    lv_obj_set_style_pad_all(panel_,12,0);
    lv_obj_set_style_pad_column(panel_,14,0);
    lv_obj_set_flex_flow(panel_,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel_,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(panel_,LV_OBJ_FLAG_CLICKABLE);   // a tap between buttons stays inside
    lv_obj_remove_flag(panel_,LV_OBJ_FLAG_SCROLLABLE);
    auto panel_button=[&](PanelButton& b) {
        b.obj=lv_obj_create(panel_);
        lv_obj_remove_style_all(b.obj);
        lv_obj_set_size(b.obj,84,76);
        lv_obj_set_style_radius(b.obj,16,0);
        lv_obj_set_style_bg_opa(b.obj,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(b.obj,lv_color_hex(0x2C3038),0);
        lv_obj_set_style_border_color(b.obj,lv_color_hex(0xFFFFFF),LV_STATE_PRESSED);
        lv_obj_set_style_border_opa(b.obj,LV_OPA_70,LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b.obj,3,LV_STATE_PRESSED);
        lv_obj_add_flag(b.obj,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b.obj,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b.obj,OnPanelButton,LV_EVENT_CLICKED,this);
        b.icon=lv_label_create(b.obj);
        lv_obj_set_style_text_font(b.icon,&font_awesome_30_4,0);
        lv_obj_set_style_text_color(b.icon,lv_color_hex(0xFFFFFF),0);
        lv_label_set_text(b.icon,"");
        lv_obj_center(b.icon);
        lv_obj_set_style_transform_pivot_x(b.icon,lv_pct(50),0);
        lv_obj_set_style_transform_pivot_y(b.icon,lv_pct(50),0);
        Visible(b.obj,false);
    };
    panel_button(call_btn_);
    panel_button(mic_btn_);
    panel_button(setup_btn_);
    Visible(scrim_,false);
    Visible(panel_,false);
#endif
#if CONFIG_EIDOLON_EXPRESSION_DIAGNOSTICS
    diagnostic_=Label(viewport_,&lv_font_montserrat_14,LV_ALIGN_BOTTOM_LEFT,4,-2);
    lv_obj_set_width(diagnostic_,layout.face.w-8);
    lv_obj_set_style_text_color(diagnostic_,lv_color_hex(palette::Paper),0);
#if CONFIG_EIDOLON_PET_FACE
    lv_obj_set_style_text_color(diagnostic_,lv_color_hex(palette::Ink),0);
#endif
    lv_obj_set_style_text_opa(diagnostic_,LV_OPA_70,0);
    lv_obj_set_height(diagnostic_,32);
    lv_label_set_long_mode(diagnostic_,LV_LABEL_LONG_CLIP);
    Visible(diagnostic_,false);
#endif
}
void CompanionFaceView::SetContentFont(const lv_font_t* font) {
    if (!font || !subtitle_ || font_==font) return;
    font_=font;
    lv_obj_set_style_text_font(subtitle_,font_,0);
    lv_obj_set_style_text_font(detail_,font_,0);
    // Reflow from a whole page after a resource font changes its metrics.
    displayed_subtitle_.clear();
    subtitle_idle_=false;
    detail_scroll_start_=NowMs();
    lv_obj_scroll_to_y(detail_panel_,0,LV_ANIM_OFF);
    UpdateLayout();
    RefreshSubtitle(NowMs());
}
void CompanionFaceView::SetSystemStatus(const char* text) {
    if (!display_ || !status_) return;
    DisplayLockGuard lock(display_);
    // Once projection starts, it is the sole lifecycle state owner.
    if (status_ && !has_model_) lv_label_set_text(status_,text ? text : "");
}
void CompanionFaceView::ShowNotification(const char* text,int duration_ms) {
    if (!display_ || !notification_) return;
    DisplayLockGuard lock(display_);
    notification_until_=NowMs()+std::clamp(duration_ms,1,15000);
    Text(notification_,text);
    Visible(notification_,text && *text);
}
void CompanionFaceView::ShowCameraCapture() {
#if CONFIG_EIDOLON_PET_FACE
    if (!display_) return;
    DisplayLockGuard lock(display_);
    pet_.OnCameraShutter(NowMs());
#endif
    // Only the pet takes stills today (the boards that declare camera.capture);
    // a companion face that gains a camera gets its shutter with it.
}
void CompanionFaceView::SetMotionDiagnostic(const char* name,const char* status,const char* reason) {
#if CONFIG_EIDOLON_PET_FACE
    if (!display_) return;
    {
        DisplayLockGuard lock(display_);
        const char* st=status ? status : "";
        if (!std::strcmp(st,"COMPLETED")) PetAction(pet::HostAction::Done);
        else if (!std::strcmp(st,"ACCEPTED") || !std::strcmp(st,"STARTED") || !std::strcmp(st,"RUNNING")) PetAction(pet::HostAction::Running);
        else PetAction(pet::HostAction::Failed);
    }
    return;   // no F:/M: text on the pet
#endif
    if (!display_ || !diagnostic_) return;
    DisplayLockGuard lock(display_);
    motion_diagnostic_=std::string(name ? name : "-")+":"+(status ? status : "-");
    if (reason && *reason && std::strcmp(reason,"SEQUENCE_FINISHED")) motion_diagnostic_+="/"+std::string(reason);
    diagnostic_until_=NowMs()+5000;RefreshDiagnostic();
}
void CompanionFaceView::RefreshDiagnostic() {
    if (!diagnostic_) return;
    const auto text="F:"+face_diagnostic_+"\nM:"+motion_diagnostic_;
    Text(diagnostic_,text.c_str());
#if CONFIG_EIDOLON_PET_FACE
    Visible(diagnostic_,false);   // the status-bar action icon carries receipts
#else
    Visible(diagnostic_,NowMs()<diagnostic_until_);
#endif
}
void CompanionFaceView::RefreshInformation() {
    RefreshDiagnostic();
    if (notification_until_ && NowMs()>=notification_until_) {
        notification_until_=0;
        Text(notification_,"");
        Visible(notification_,false);
    }
    Text(information_,information_text_.c_str());
    UpdateLayout();
}
void CompanionFaceView::ClearSubtitle() {
    pending_subtitle_.clear();
    displayed_subtitle_.clear();
    Text(subtitle_,"");
    lv_obj_scroll_to_y(subtitle_panel_,0,LV_ANIM_OFF);
    subtitle_idle_=false;
}
void CompanionFaceView::RefreshSubtitle(uint64_t now) {
    if (!dialogue_visible_ || !CurrentOutputGate().Allows(presentation::Output::DialogueText)) {
        dialogue_visible_=false;
        ClearSubtitle();
        return;
    }
    // Coalesce bursts to at most ten visible text updates per second. The
    // first phrase is immediate; duplicate models do not restart reading.
    if (pending_subtitle_ != displayed_subtitle_ &&
        (displayed_subtitle_.empty() || now-subtitle_updated_>=100)) {
        const bool appended = !displayed_subtitle_.empty() &&
            pending_subtitle_.compare(0,displayed_subtitle_.size(),displayed_subtitle_)==0;
        Text(subtitle_,pending_subtitle_.c_str());
        lv_obj_update_layout(subtitle_panel_);
        lv_point_t size;
        const int page_height=std::max<int>(1,(lv_obj_get_height(subtitle_panel_)+4)/(lv_font_get_line_height(font_)+4)) * (lv_font_get_line_height(font_)+4);
        lv_text_get_size(&size,pending_subtitle_.c_str(),font_,0,4,lv_obj_get_width(subtitle_panel_),LV_TEXT_FLAG_NONE);
        // Pad the final page so scrolling can stop on a whole-line boundary.
        const int pages=std::max<int>(1,(size.y+4+page_height-1)/page_height);
        const int line_height=lv_font_get_line_height(font_)+4;
        const int lines=std::max<int>(1,(size.y+4+line_height-1)/line_height);
        const int visible_lines=std::max<int>(1,(lv_obj_get_height(subtitle_panel_)+4)/line_height);
        const int tail_y=std::max(0,lines-visible_lines)*line_height;
        const int end_y=agent_speaking_ ? tail_y : (pages-1)*page_height;
        lv_obj_set_style_pad_bottom(subtitle_,std::max<int>(0,end_y+lv_obj_get_height(subtitle_panel_)-size.y),0);
        displayed_subtitle_=pending_subtitle_;
        subtitle_updated_=now;
        subtitle_idle_=false;
        if (agent_speaking_) {
            // Keep the newest spoken line visible, on whole-line boundaries.
            lv_obj_scroll_to_y(subtitle_panel_,tail_y,LV_ANIM_OFF);
            subtitle_page_at_=now;
        } else if (!appended) {
            lv_obj_scroll_to_y(subtitle_panel_,0,LV_ANIM_OFF);
            subtitle_page_at_=now;
        }
    }
    if (displayed_subtitle_.empty()) return;
    lv_obj_update_layout(subtitle_panel_);
    const int remaining=lv_obj_get_scroll_bottom(subtitle_panel_);
    if (!agent_speaking_ && remaining>0 && now-subtitle_page_at_>=3500) {
        const int line_height=lv_font_get_line_height(font_)+4;
        // Move by whole lines; the last page must not begin with half a line.
        const int page_lines=std::max<int>(1,(lv_obj_get_height(subtitle_panel_)+4)/line_height);
        lv_obj_scroll_to_y(subtitle_panel_,lv_obj_get_scroll_y(subtitle_panel_)+page_lines*line_height,LV_ANIM_OFF);
        subtitle_page_at_=now;
        subtitle_idle_=false;
    }
    if (agent_speaking_ || lv_obj_get_scroll_bottom(subtitle_panel_)>0) {
        subtitle_idle_=false;
    } else if (!subtitle_idle_) {
        subtitle_idle_=true;
        subtitle_idle_since_=now;
    } else if (now-subtitle_idle_since_>=6000) {
        // Keep the consumed source for deduplication. A periodic Render of the
        // same model must not resurrect text that has finished its reading time.
        Text(subtitle_,"");
    }
}
void CompanionFaceView::UpdateLayout() {
    if (!viewport_) return;
    auto* parent=lv_obj_get_parent(viewport_);
    const bool information=!information_text_.empty();
    // Reserve caption space for the whole conversation, including pauses.
    const companion::Layout layout(lv_obj_get_width(parent),lv_obj_get_height(parent),has_actions_,dialogue_visible_ || information,
        dialogue_visible_ ? std::max<int>(52,2*lv_font_get_line_height(font_)+4) : 52);
    lv_obj_set_pos(viewport_,layout.face.x,layout.face.y);
    lv_obj_set_size(viewport_,layout.face.w,layout.face.h);
    lv_obj_set_pos(information_,layout.information.x,layout.information.y);
    lv_obj_set_pos(subtitle_panel_,layout.information.x,layout.information.y);
    lv_obj_set_height(subtitle_panel_,layout.information.h);
    lv_obj_set_pos(caption_card_,4,layout.information.y-4);
    lv_obj_set_size(caption_card_,lv_obj_get_width(parent)-8,layout.information.h+8);
    Visible(caption_card_,dialogue_visible_ || information);
    Visible(information_,information);
    Visible(subtitle_panel_,dialogue_visible_);
    // On a detail page the toast uses the footer, leaving recovery instructions readable.
    lv_obj_set_pos(notification_,12,lv_obj_is_visible(detail_panel_) ? layout.information.y : 36);
    int right=lv_obj_get_width(parent)-12;
    for (auto* icon : {indicators_.battery,indicators_.network,indicators_.microphone,setup_}) {
        const bool visible=icon==setup_ ? lv_obj_is_visible(setup_) : lv_label_get_text(icon)[0]!='\0';
        if (icon!=setup_) Visible(icon,visible);
        if (!visible) continue;
        right-=28;
        lv_obj_set_pos(icon,right,4);
    }
#if CONFIG_EIDOLON_PET_FACE
    lv_obj_set_width(status_,std::max(28,right-20));
#else
    lv_obj_set_width(status_,std::max(28,right-34));
#endif
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
    if (!viewport_) return;
    RefreshSubtitle(NowMs());
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
#if CONFIG_EIDOLON_PET_FACE
    const bool visible=lv_obj_is_visible(pet_skin_.obj());
    Emit(runtime_.SetVisible(visible));
    // The pet keeps living (LEDs, senses, mood) behind system pages.
    if (!visible) { awaiting_frame_=false;AdvancePet(NowMs(),false);return; }
#else
    const bool visible=lv_obj_is_visible(avatar_->getPanel()->get());
    Emit(runtime_.SetVisible(visible));
    if (!visible) { awaiting_frame_=false;return; }
#endif
    if (awaiting_frame_) {
        if (completed_frame != prepared_frame_) return;
        awaiting_frame_=false;
        Emit(runtime_.Presented());
    }
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
    Benchmark(NowMs());
#endif
#if CONFIG_EIDOLON_PET_FACE
    {
        const auto now=NowMs();
        runtime_.Tick(now);   // still owns the plan clock and the receipt frames
        AdvancePet(now,runtime_.active());
        if (!runtime_.active()) return;
        // AdvancePet invalidated the face, so this frame number reaches a real flush.
        ++prepared_frame_;
        awaiting_frame_=true;
        return;
    }
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
    if (event.token==diagnostic_token_ && event.status!=expression::Status::None) {
        const char* state="failed";
        switch(event.status) {
        case expression::Status::Accepted:state="accepted";break;
        case expression::Status::Started:state="started";break;
        case expression::Status::Completed:state="completed";break;
        case expression::Status::Cancelled:state="cancelled";break;
        case expression::Status::Rejected:state="rejected";break;
        default:break;
        }
        face_diagnostic_=face_diagnostic_.substr(0,face_diagnostic_.find(':'))+":"+state;
        diagnostic_until_=NowMs()+5000;RefreshDiagnostic();
#if CONFIG_EIDOLON_PET_FACE
        if (event.status==expression::Status::Completed) PetAction(pet::HostAction::Done);
        else if (event.status==expression::Status::Accepted || event.status==expression::Status::Started) PetAction(pet::HostAction::Running);
        else PetAction(pet::HostAction::Failed);
#endif
    }
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
    auto event=runtime_.Submit(plan,NowMs());
    if (event.status==expression::Status::Accepted) {
        diagnostic_token_=plan.token;
        face_diagnostic_=std::string(expression::kGestureNames[static_cast<unsigned>(plan.steps[0].gesture)]);
        if (NowMs()>=diagnostic_until_) motion_diagnostic_="-";
    }
    Emit(event);return event;
}
expression::Event CompanionFaceView::Cancel(uint64_t token) {
    DisplayLockGuard lock(display_);
    auto event=runtime_.Cancel(token);Emit(event);return event;
}
void CompanionFaceView::PulseEmotion(const char* emotion,int ttl_ms) {
#if CONFIG_EIDOLON_PET_FACE
    // body.presence.set drives the pet's mood instead of a one-shot pulse; "sleepy"
    // (legacy) and "sad" both mean the owner left (design §11).
    if (!emotion) return;
    (void)ttl_ms;
    DisplayLockGuard pet_lock(display_);
    if (!std::strcmp(emotion,"happy")) pet_.OnPresence(true,NowMs());
    else if (!std::strcmp(emotion,"sad") || !std::strcmp(emotion,"sleepy")) pet_.OnPresence(false,NowMs());
    return;
#endif
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
    if (!viewport_) return;
    DisplayLockGuard lock(display_);
    expression::BaseState base=expression::BaseState::Idle;
    if (model.severity==UiSeverity::Error) base=expression::BaseState::Error;
    else if (model.turn==TurnPhase::AgentThinking || model.turn==TurnPhase::Committing)
        base=expression::BaseState::Thinking;
    else if (model.turn==TurnPhase::UserSpeaking || model.turn==TurnPhase::Recording)
        base=expression::BaseState::Listening;
    runtime_.SetBase(base);
    has_model_=true;
#if !CONFIG_EIDOLON_PET_FACE   // the pet sets its microphone switch in RenderPetChrome
    Text(indicators_.microphone,model.show_mute_icon ? FONT_AWESOME_MICROPHONE_SLASH : "");
#endif
#if CONFIG_EIDOLON_PET_FACE
    dialogue_visible_=false;   // no captions on the pet: the face is the conversation
#else
    dialogue_visible_=model.scene==UiScene::Conversation &&
        CurrentOutputGate().Allows(presentation::Output::DialogueText);
#endif
    agent_speaking_=model.turn==TurnPhase::AgentSpeaking;
    const auto color=model.severity==UiSeverity::Error ? palette::Error :
        model.severity==UiSeverity::Attention ? palette::Attention : palette::Accent;
    lv_obj_set_style_bg_color(state_dot_,lv_color_hex(color),0);
    lv_obj_set_style_text_color(status_,lv_color_hex(color),0);
    Text(status_,model.status_text);
    Text(mode_,model.mode_label);
    const bool touch=model.primary_presentation==UiActionPresentation::TouchControl;
    const bool hint=model.primary_presentation==UiActionPresentation::InputHint;
    primary_intent_=touch && model.primary_enabled ? model.primary_intent : UiIntent::None;
#if CONFIG_EIDOLON_PET_FACE
    // Tapping the face opens a conversation and the status-bar microphone mutes it;
    // no big button covers the pet (push-to-talk keeps its hold button).
    const bool primary_visible=primary_intent_!=UiIntent::None && primary_intent_!=UiIntent::OpenConversation &&
        primary_intent_!=UiIntent::ToggleMicrophone;
#else
    const bool primary_visible=primary_intent_!=UiIntent::None;
#endif
#if CONFIG_EIDOLON_PET_FACE
    // The corner control floats over the face, so it reserves no action row.
    has_actions_=primary_visible || hint;
#else
    has_actions_=primary_visible || model.show_end_action || hint;
#endif
    Text(hint_action_,hint ? model.input_hint.action : "");
    Text(hint_key_,hint ? model.input_hint.key : "");
    Text(hint_effect_,hint ? model.input_hint.effect : "");
    Visible(input_hint_,hint);
    Visible(mode_,touch || model.show_end_action);
    Text(primary_label_,model.primary_label);
    Visible(primary_,primary_visible);
    Visible(close_,model.show_end_action);
    Visible(setup_,model.show_setup_action);
    const bool face=model.scene==UiScene::Ready || model.scene==UiScene::Conversation ||
        model.scene==UiScene::OpeningConversation || model.scene==UiScene::Ended;
    Visible(viewport_,face);Visible(detail_panel_,!face);
#if CONFIG_EIDOLON_PET_FACE
    Visible(pet_skin_.obj(),face);
    RenderPetChrome(model,face);
    pet::Phase phase=pet::Phase::Ambient;
    if (!face) phase=pet::Phase::System;
    else if (model.severity==UiSeverity::Error) phase=pet::Phase::Error;
    else if (model.scene==UiScene::OpeningConversation) phase=pet::Phase::Opening;
    else if (model.scene==UiScene::Conversation) {
        switch (model.turn) {
        case TurnPhase::Committing: case TurnPhase::AgentThinking: phase=pet::Phase::Thinking; break;
        case TurnPhase::AgentSpeaking: phase=pet::Phase::Responding; break;
        default: phase=pet::Phase::Listening; break;   // idle turn: the microphone is open
        }
    }
    if (phase!=pet_phase_) ESP_LOGI("pet","phase %d -> %d",static_cast<int>(pet_phase_),static_cast<int>(phase));
    // It stopped speaking: the microphone opens about a second from now, and from then on
    // nothing may move. Bring the head home now rather than leave a speaking move half done.
    if (pet_phase_==pet::Phase::Responding && phase!=pet::Phase::Responding) Board::GetInstance().SettlePetHead();
    pet_phase_=phase;
#endif
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
#if CONFIG_EIDOLON_PET_FACE
    // The status bar icon carries the session state; nothing sits under the pet.
    if (face) information_text_.clear();
#endif
    // The reachable start action already explains how to begin. Keep the
    // ready face free of a second, generic invitation to do the same thing.
    if (model.scene==UiScene::Ready && has_actions_) information_text_="";
    if (dialogue_visible_) {
        pending_subtitle_=model.subtitle ? model.subtitle : "";
        if (pending_subtitle_.empty()) ClearSubtitle();
    }
    if (!face) {
        lv_obj_update_layout(detail_panel_);
        information_text_=lv_obj_get_height(detail_)>lv_obj_get_height(detail_panel_) ? (touch_navigation_ ? "Swipe to read more" : "") : "";
    }
    UpdateLayout();
    RefreshSubtitle(NowMs());
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
#if CONFIG_EIDOLON_PET_FACE
// ---- Desk pet ----------------------------------------------------------------------------
namespace {
constexpr const char* kPetTag="pet";
float MicLevel() {
    const uint32_t ppm=eidolon_livekit_board_recent_capture_rms_ppm();
    if (ppm<10) return 0;
    const float dbfs=20.f*std::log10(static_cast<float>(ppm)/1e6f);
    return std::clamp((dbfs+55.f)/35.f,0.f,1.f);   // -55 dBFS silent .. -20 dBFS loud
}
// The companion's own voice as it leaves the speaker; the runtime scales it to the voice's peak.
float SpeechDbfs() {
    const uint32_t ppm=eidolon_livekit_board_recent_playback_rms_ppm();
    return ppm<10 ? -100.f : 20.f*std::log10(static_cast<float>(ppm)/1e6f);
}
constexpr const char* kPhaseNames[7]={"ambient","opening","listen","think","speak","error","system"};
}  // namespace
void CompanionFaceView::RenderPetChrome(const EidolonUiModel& model, bool face) {
    // Status bar: state only. Words return only for problems; a red microphone
    // shows when muted. Nothing on the bar is a button of its own.
    const bool busy=model.scene==UiScene::Ended && model.end_reason==EndReason::Busy;
    if (busy) Text(status_,"Device busy");
    Visible(status_,!face || model.severity!=UiSeverity::Normal || busy);
    Visible(mode_,false);
    Visible(close_,false);
    Visible(setup_,false);
    pet_muted_=model.show_mute_icon;
    Text(indicators_.microphone,model.show_mute_icon ? FONT_AWESOME_MICROPHONE_SLASH : "");
    // Panel actions, shown only while they apply.
    session_button_intent_=UiIntent::None;
    if (face && model.show_end_action) session_button_intent_=UiIntent::CloseConversation;
#if CONFIG_EIDOLON_PET_SESSION_BUTTON
    else if (face && primary_intent_==UiIntent::OpenConversation) session_button_intent_=UiIntent::OpenConversation;
#endif
    const bool join=session_button_intent_==UiIntent::OpenConversation;
    call_btn_.intent=session_button_intent_;
    Text(call_btn_.icon,FONT_AWESOME_PHONE);
    lv_obj_set_style_bg_color(call_btn_.obj,lv_color_hex(join ? 0x2F7A5C : 0xB4473F),0);
    lv_obj_set_style_transform_rotation(call_btn_.icon,join ? 0 : 1350,0);   // hang-up handset
    const bool live=model.scene==UiScene::Conversation || model.scene==UiScene::OpeningConversation;
    mic_btn_.intent=face && live && primary_intent_==UiIntent::ToggleMicrophone ? UiIntent::ToggleMicrophone : UiIntent::None;
    Text(mic_btn_.icon,model.show_mute_icon ? FONT_AWESOME_MICROPHONE_SLASH : FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_bg_color(mic_btn_.obj,lv_color_hex(model.show_mute_icon ? 0xB4473F : 0x2C3038),0);
    setup_btn_.intent=model.show_setup_action ? UiIntent::OpenSetup : UiIntent::None;
    Text(setup_btn_.icon,FONT_AWESOME_GEAR);
    for (auto* b : {&call_btn_,&mic_btn_,&setup_btn_}) Visible(b->obj,b->intent!=UiIntent::None);
    const bool actions=PanelHasActions();
    Visible(handle_,actions);
    if (panel_open_ && !actions) ClosePanel();
}
bool CompanionFaceView::PanelHasActions() const {
    return call_btn_.intent!=UiIntent::None || mic_btn_.intent!=UiIntent::None || setup_btn_.intent!=UiIntent::None;
}
void CompanionFaceView::OpenPanel() {
    if (!panel_ || !PanelHasActions()) return;
    panel_until_=NowMs()+5000;
    if (panel_open_) return;
    panel_open_=true;
    lv_obj_move_foreground(scrim_);
    lv_obj_move_foreground(panel_);
    Visible(scrim_,true);
    Visible(panel_,true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a,panel_);
    lv_anim_set_values(&a,-lv_obj_get_height(panel_),0);
    lv_anim_set_duration(&a,160);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a,[](void* obj,int32_t y) { lv_obj_set_y(static_cast<lv_obj_t*>(obj),y); });
    lv_anim_start(&a);
    ESP_LOGI("pet","control panel open");
}
void CompanionFaceView::ClosePanel() {
    if (!panel_open_) return;
    panel_open_=false;
    lv_anim_delete(panel_,nullptr);
    Visible(scrim_,false);
    Visible(panel_,false);
}
void CompanionFaceView::OnBar(lv_event_t* event) {
    auto* self=static_cast<CompanionFaceView*>(lv_event_get_user_data(event));
    lv_point_t point{};
    if (auto* indev=lv_indev_active()) lv_indev_get_point(indev,&point);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: self->bar_press_y_=point.y; break;
    case LV_EVENT_PRESSING: if (point.y-self->bar_press_y_>18) self->OpenPanel(); break;   // dragged down
    case LV_EVENT_CLICKED: self->OpenPanel(); break;
    default: break;
    }
}
void CompanionFaceView::OnScrim(lv_event_t* event) {
    static_cast<CompanionFaceView*>(lv_event_get_user_data(event))->ClosePanel();
}
void CompanionFaceView::OnPanelButton(lv_event_t* event) {
    auto* self=static_cast<CompanionFaceView*>(lv_event_get_user_data(event));
    auto* target=static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    UiIntent intent=UiIntent::None;
    for (auto* b : {&self->call_btn_,&self->mic_btn_,&self->setup_btn_}) if (b->obj==target) intent=b->intent;
    self->ClosePanel();
    if (intent==UiIntent::None) return;
    ESP_LOGI("pet","control panel action %d",static_cast<int>(intent));
    DispatchEidolonUiIntent(intent);
}
void CompanionFaceView::PetAction(pet::HostAction action) {
    pet_.OnHostAction(action,NowMs());
}
void CompanionFaceView::BuildPet(const BuildContext& ctx) {
    pet_skin_.Build(ctx.parent,ctx.font);
    for (auto code : {LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_RELEASED,LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(pet_skin_.obj(),OnPetTouch,code,this);
    pet_senses_=xQueueCreate(24,sizeof(pet::SenseEvent));
}
void CompanionFaceView::PostSense(const pet::SenseEvent& event) {
    if (pet_senses_) xQueueSend(pet_senses_,&event,0);   // drop when full: senses are lossy by design
}
void CompanionFaceView::OnPetTouch(lv_event_t* event) {
    auto* self=static_cast<CompanionFaceView*>(lv_event_get_user_data(event));
    const auto code=lv_event_get_code(event);
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING && code!=LV_EVENT_RELEASED && code!=LV_EVENT_PRESS_LOST) return;
    lv_point_t point{};
    if (auto* indev=lv_indev_active()) lv_indev_get_point(indev,&point);
    const auto now=NowMs();
    if (code==LV_EVENT_PRESSED) {
        self->pet_pointer_=true;self->pet_travel_=0;self->pet_press_ms_=now;self->pet_last_point_=point;
        self->pet_px_=point.x;self->pet_py_=point.y;
        self->pet_.OnScreenStroke(true,point.x,0,now);
        return;
    }
    if (code==LV_EVENT_PRESSING) {
        self->pet_travel_+=std::abs(point.x-self->pet_last_point_.x)+std::abs(point.y-self->pet_last_point_.y);
        self->pet_last_point_=point;self->pet_px_=point.x;self->pet_py_=point.y;
        self->pet_.OnScreenStroke(true,point.x,self->pet_travel_,now);
        return;
    }
    self->pet_pointer_=false;
    self->pet_.OnScreenStroke(false,self->pet_px_,self->pet_travel_,now);
    if (code==LV_EVENT_RELEASED && self->pet_travel_<24 && now-self->pet_press_ms_<450) {
        self->pet_.OnScreenTap(now);
        ESP_LOGI(kPetTag,"face tap (intent=%d)",static_cast<int>(self->primary_intent_));
        if (self->primary_intent_==UiIntent::OpenConversation) DispatchEidolonUiIntent(UiIntent::OpenConversation);
    }
}
void CompanionFaceView::PetSenses(uint64_t now) {
    auto& board=Board::GetInstance();
    if (!pet_sink_registered_) {
        // Registered from the first tick, after the board finished constructing.
        pet_sink_registered_=true;
        board.SetPetSenseSink([this](const pet::SenseEvent& event) { PostSense(event); });
    }
    pet::SenseEvent event;
    while (pet_senses_ && xQueueReceive(pet_senses_,&event,0)==pdTRUE) {
        // The speaker shakes its own body: a "knock" while the pet talks is itself.
        if (event.kind==pet::SenseKind::Knock && pet_phase_==pet::Phase::Responding) continue;
        if (event.kind==pet::SenseKind::LightsOff) pet_dark_=true;
        else if (event.kind==pet::SenseKind::LightsOn) pet_dark_=false;
        if (event.kind!=pet::SenseKind::TouchMove)
            ESP_LOGI(kPetTag,"sense %s v=%d d=%d",pet::SenseKindName(event.kind),static_cast<int>(event.value*100),event.dir);
        event.t_ms=now;
        pet_.OnSense(event);
    }
    if (now>=pet_next_power_ms_) {
        pet_next_power_ms_=now+5000;
        int level=0;bool charging=false;
        if (board.GetPetPower(level,charging)) {
            if (charging!=pet_charging_ || level!=pet_battery_ || pet_battery_<0) {
                if (charging!=pet_charging_ || pet_battery_<0) ESP_LOGI(kPetTag,"power external=%d battery=%d%%",charging,level);
                pet_charging_=charging;pet_battery_=level;
                pet_.OnPower(charging,level,now);
            }
        }
    }
}
void CompanionFaceView::PetOutputs(uint64_t now) {
    auto& board=Board::GetInstance();
    if (now>=pet_next_led_ms_) {
        pet_next_led_ms_=now+50;
        uint8_t rgb[36];
        pet_.Led(now,rgb);
        board.SetPetLeds(rgb);
    }
    pet::Chirp chirp;
    if (pet_.TakeChirp(chirp)) {
        // Chirps only outside a conversation: while it listens or thinks the microphone is
        // open (a chirp would be heard as speech), and while it speaks the speaker is its
        // voice's. Then the Owner's standing permission for audio cues decides.
        const bool idle=pet_phase_==pet::Phase::Ambient;
        const bool allowed=idle && CurrentOutputGate().AllowsAmbient(presentation::Output::AudioCue);
        const bool played=allowed && pet::PlayChirp(chirp,pet_dark_ ? .45f : 1.f);
        ESP_LOGI(kPetTag,"chirp %s idle=%d allowed=%d played=%d",pet::ChirpName(chirp),idle,allowed,played);
    }
    pet::HeadClip clip;
    if (pet_.TakeHeadClip(clip)) {
        // Local life moves the head only where the Owner allows motion (inside a
        // session: the session plan). The body still refuses while capture holds it
        // or a server motion owns the head.
        const bool allowed=CurrentOutputGate().AllowsAmbient(presentation::Output::Motion);
        const bool played=allowed && board.PlayPetHeadClip(clip);
        ESP_LOGI(kPetTag,"head clip p%u keys=%u allowed=%d played=%d (%s)",clip.priority,
                 static_cast<unsigned>(clip.count),allowed,played,pet_.activity());
    }
    if (now>=pet_next_stats_ms_) {
        // Self-check line: how often the face redrew and what it cost the LVGL task.
        if (pet_next_stats_ms_) {
            uint32_t draws=0,total_us=0,max_us=0,frames=0,frame_us=0,frame_max=0;
            pet_skin_.TakeStats(draws,total_us,max_us);
            pet_skin_.TakeFrameStats(frames,frame_us,frame_max);
            ESP_LOGI(kPetTag,"stats 30s: fps=%u.%u frame_ms avg=%u max=%u draws=%u avg_task_us=%u max_task_us=%u mood=%s phase=%d dark=%d led_owner=1 heap_int=%u",
                     static_cast<unsigned>(frames/30),static_cast<unsigned>((frames%30)*10/30),
                     static_cast<unsigned>(frames ? frame_us/frames/1000 : 0),static_cast<unsigned>(frame_max/1000),
                     static_cast<unsigned>(draws),static_cast<unsigned>(draws ? total_us/draws : 0),static_cast<unsigned>(max_us),
                     pet::MoodName(pet_.mood()),static_cast<int>(pet_phase_),pet_dark_,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
            // Per phase: frames drawn @ average frame time. A conversation's smoothness is the
            // listen / think / speak figures (1000 / ms is the most frames it could show).
            char line[160];size_t n=0;line[0]=0;
            for (size_t i=0;i<7 && n<sizeof(line);++i) {
                if (!pet_phase_frames_[i]) continue;
                const int w=std::snprintf(line+n,sizeof(line)-n," %s=%u@%ums",kPhaseNames[i],
                                          static_cast<unsigned>(pet_phase_frames_[i]),
                                          static_cast<unsigned>(pet_phase_us_[i]/pet_phase_frames_[i]/1000));
                if (w>0) n+=static_cast<size_t>(w);
            }
            ESP_LOGI(kPetTag,"stats 30s by phase:%s",n ? line : " -");
        }
        std::fill(std::begin(pet_phase_frames_),std::end(pet_phase_frames_),0u);
        std::fill(std::begin(pet_phase_us_),std::end(pet_phase_us_),uint64_t{0});
        pet_next_stats_ms_=now+30000;
    }
    if (pet_.mood()!=pet_logged_mood_) {
        ESP_LOGI(kPetTag,"mood %s -> %s",pet::MoodName(pet_logged_mood_),pet::MoodName(pet_.mood()));
        pet_logged_mood_=pet_.mood();
    }
}
void CompanionFaceView::AdvancePet(uint64_t now, bool host_frame) {
    PetSenses(now);
    pet::RuntimeInputs in;
    in.now_ms=now;
    in.phase=pet_phase_;
    in.mic_level=pet_phase_==pet::Phase::Listening ? MicLevel() : 0.f;
    in.speech_dbfs=pet_phase_==pet::Phase::Responding ? SpeechDbfs() : -100.f;
    {   // Frames drawn since the last tick belong to the phase that was showing.
        uint32_t frames=0;uint64_t us=0;
        pet_skin_.FrameTotals(frames,us);
        const size_t i=std::min<size_t>(static_cast<size_t>(pet_phase_),6);
        pet_phase_frames_[i]+=frames-pet_seen_frames_;pet_phase_us_[i]+=us-pet_seen_us_;
        pet_seen_frames_=frames;pet_seen_us_=us;
    }
    expression::Gesture gesture=expression::Gesture::Attend;float amount=0;
    in.host_active=runtime_.Current(now,gesture,amount);
    in.host_gesture=static_cast<uint8_t>(gesture);
    in.host_amount=amount;
    in.pointer=pet_pointer_;in.pointer_x=pet_px_;in.pointer_y=pet_py_;
    in.dark=pet_dark_;
    in.muted=pet_muted_;
    if (panel_open_ && now>=panel_until_) ClosePanel();
    pet::Frame frame=pet_.Tick(in);
    const bool card=pet_.card_requested(now);
    if (card && !pet_card_shown_) {
        char line1[48],line2[48];
        if (pet_battery_>=0) std::snprintf(line1,sizeof(line1),pet_charging_ ? "%d%% on power" : "%d%% battery",pet_battery_);
        else std::snprintf(line1,sizeof(line1),"%s","--");
        std::snprintf(line2,sizeof(line2),"feeling %s",pet::MoodName(pet_.mood()));
        pet_skin_.SetCard(line1,line2);
    }
    pet_card_shown_=card;
    // ~30 fps cap for local life (SPI + software render share the CPU with audio);
    // a Host plan frame is never skipped, its receipt needs the flush.
    if (host_frame || now-pet_last_show_ms_>=33) {
        if (pet_skin_.Show(frame,host_frame)) pet_last_show_ms_=now;
    }
    PetOutputs(now);
}
#endif
}
