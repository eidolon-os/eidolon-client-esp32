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
    viewport_=panel(layout.face);
    avatar_=std::make_unique<stackchan::avatar::DefaultAvatar>();
    avatar_->primaryColor=lv_color_hex(palette::Expression);
    avatar_->secondaryColor=lv_color_hex(palette::Stage);
    avatar_->init(viewport_,ctx.font,false);
    auto* stage=avatar_->getPanel()->get();
    lv_obj_set_style_radius(stage,22,0);
    lv_obj_set_style_border_width(stage,1,0);
    lv_obj_set_style_border_color(stage,lv_color_hex(palette::StageEdge),0);
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
    input_hint_=Label(ctx.parent,ctx.font,LV_ALIGN_TOP_LEFT,12,layout.actions.y+2);
    lv_obj_set_size(input_hint_,layout.actions.w,28);
    lv_label_set_long_mode(input_hint_,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(input_hint_,lv_color_hex(palette::Muted),0);
    lv_obj_set_style_bg_color(input_hint_,lv_color_hex(palette::Soft),0);
    lv_obj_set_style_bg_opa(input_hint_,LV_OPA_COVER,0);
    lv_obj_set_style_radius(input_hint_,10,0);
    lv_obj_set_style_text_align(input_hint_,LV_TEXT_ALIGN_CENTER,0);
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
#if CONFIG_EIDOLON_EXPRESSION_DIAGNOSTICS
    diagnostic_=Label(viewport_,&lv_font_montserrat_14,LV_ALIGN_BOTTOM_LEFT,4,-2);
    lv_obj_set_width(diagnostic_,layout.face.w-8);
    lv_obj_set_style_text_color(diagnostic_,lv_color_hex(palette::Paper),0);
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
void CompanionFaceView::SetMotionDiagnostic(const char* name,const char* status,const char* reason) {
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
    Visible(diagnostic_,NowMs()<diagnostic_until_);
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
    lv_obj_set_width(status_,std::max(28,right-34));
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
    Text(indicators_.microphone,model.show_mute_icon ? FONT_AWESOME_MICROPHONE_SLASH : "");
    dialogue_visible_=model.scene==UiScene::Conversation &&
        CurrentOutputGate().Allows(presentation::Output::DialogueText);
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
    has_actions_=primary_intent_!=UiIntent::None || model.show_end_action || hint;
    Text(input_hint_,hint ? model.input_hint : "");
    Visible(input_hint_,hint);
    Visible(mode_,touch || model.show_end_action);
    Text(primary_label_,model.primary_label);
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
}
