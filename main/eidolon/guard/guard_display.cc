#include "guard/guard_display.h"

#include <algorithm>
#include <cstdio>
#include "display.h"
#include "display/lcd_display.h"

namespace eidolon {
namespace {

const char* LabelForState(GuardState state, GuardFaultCode fault)
{
    switch (state) {
    case GuardState::Idle:
        return "WATCHING";
    case GuardState::CandidatePending:
        return "MOTION";
    case GuardState::Candidate:
        return "CANDIDATE";
    case GuardState::AbsentPending:
        return "AWAY?";
    case GuardState::Absent:
        return "AWAY";
    case GuardState::Fault:
        return fault == GuardFaultCode::CameraUnavailable ? "CAMERA OFFLINE" : "CAMERA FAULT";
    case GuardState::Disabled:
        return "DISABLED";
    }
    return "GUARD";
}

lv_color_t ColorForState(GuardState state)
{
    switch (state) {
    case GuardState::CandidatePending:
    case GuardState::Candidate:
        return lv_color_hex(0xe0a83d);
    case GuardState::AbsentPending:
    case GuardState::Absent:
        return lv_color_hex(0x5c9db5);
    case GuardState::Fault:
        return lv_color_hex(0xc94c4c);
    case GuardState::Idle:
        return lv_color_hex(0x59ad7a);
    case GuardState::Disabled:
        return lv_color_hex(0x8a8f91);
    }
    return lv_color_hex(0x8a8f91);
}

}  // namespace

GuardDisplay::GuardDisplay(Display* display) : display_(display)
{
}

GuardDisplay::~GuardDisplay()
{
    Reset();
}

void GuardDisplay::UpdateObservation(const GuardObservation& observation)
{
    if (observation.state == GuardState::Disabled) {
        Reset();
        return;
    }
    if (EnsurePanel()) {
        ApplyState(observation);
    }
}

void GuardDisplay::Reset()
{
    if (panel_ == nullptr || display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_delete(panel_);
    panel_ = nullptr;
    state_label_ = nullptr;
    detail_label_ = nullptr;
    epoch_label_ = nullptr;
    motion_bar_ = nullptr;
}

bool GuardDisplay::EnsurePanel()
{
    if (panel_ != nullptr) {
        return true;
    }
    if (display_ == nullptr || dynamic_cast<LcdDisplay*>(display_) == nullptr ||
        !display_->IsSetupUICalled()) {
        return false;
    }

    DisplayLockGuard lock(display_);
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return false;
    }

    panel_ = lv_obj_create(screen);
    lv_obj_set_size(panel_, display_->width(), display_->height());
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_radius(panel_, 0, 0);
    lv_obj_set_style_border_width(panel_, 0, 0);
    lv_obj_set_style_pad_all(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x151918), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(panel_);
    lv_label_set_text(title, "GUARD / LOCAL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6ece7), 0);

    state_label_ = lv_label_create(panel_);
    lv_obj_set_width(state_label_, display_->width() - 24);
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_align(state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(state_label_, ColorForState(GuardState::Idle), 0);

    detail_label_ = lv_label_create(panel_);
    lv_obj_set_width(detail_label_, display_->width() - 24);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 91);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(detail_label_, lv_color_hex(0xd2dbd5), 0);

    epoch_label_ = lv_label_create(panel_);
    lv_obj_set_width(epoch_label_, display_->width() - 24);
    lv_obj_align(epoch_label_, LV_ALIGN_TOP_MID, 0, 121);
    lv_obj_set_style_text_align(epoch_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(epoch_label_, lv_color_hex(0x8d9992), 0);

    motion_bar_ = lv_bar_create(panel_);
    lv_bar_set_range(motion_bar_, 0, 64);
    lv_obj_set_size(motion_bar_, display_->width() - 24, 4);
    lv_obj_align(motion_bar_, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_set_style_bg_color(motion_bar_, lv_color_hex(0x2c332f), LV_PART_MAIN);
    lv_obj_set_style_bg_color(motion_bar_, ColorForState(GuardState::Idle), LV_PART_INDICATOR);
    lv_obj_set_style_radius(motion_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(motion_bar_, 0, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, 0, LV_ANIM_OFF);
    return true;
}

void GuardDisplay::ApplyState(const GuardObservation& observation)
{
    DisplayLockGuard lock(display_);
    if (state_label_ == nullptr || detail_label_ == nullptr || epoch_label_ == nullptr || motion_bar_ == nullptr) {
        return;
    }

    const lv_color_t color = ColorForState(observation.state);
    lv_label_set_text(state_label_, LabelForState(observation.state, observation.fault));
    lv_obj_set_style_text_color(state_label_, color, 0);
    lv_label_set_text_fmt(detail_label_, "M %lu", static_cast<unsigned long>(observation.motion_score));
    lv_label_set_text_fmt(epoch_label_, "E %lu  S %lu", static_cast<unsigned long>(observation.epoch),
                          static_cast<unsigned long>(observation.sequence));
    lv_obj_set_style_bg_color(motion_bar_, color, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, std::min<uint32_t>(observation.motion_score, 64), LV_ANIM_OFF);

}

}  // namespace eidolon
