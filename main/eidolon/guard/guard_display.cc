#include "guard/guard_display.h"

#include <algorithm>
#include <cstdio>
#include "display.h"
#include "display/lcd_display.h"

namespace eidolon {
namespace {

constexpr uint64_t kOwnerFaceResultFreshMs = 4000;

const char* LabelForState(GuardState state, GuardFaultCode fault)
{
    switch (state) {
    case GuardState::Idle:
        return "Watching for movement";
    case GuardState::CandidatePending:
        return "Checking movement...";
    case GuardState::Candidate:
        return "Activity detected";
    case GuardState::AbsentPending:
        return "Checking if area is quiet...";
    case GuardState::Absent:
        return "Area is quiet";
    case GuardState::Fault:
        return fault == GuardFaultCode::CameraUnavailable ? "Camera unavailable" : "Camera capture failed";
    case GuardState::Disabled:
        return "Guard is disabled";
    }
    return "Guard status unavailable";
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

lv_color_t OwnerColor(const GuardOwnerFaceDisplay& owner_face)
{
    if (!owner_face.profile_active || !owner_face.has_result ||
        owner_face.result_age_ms > kOwnerFaceResultFreshMs || owner_face.faces == 0) {
        return lv_color_hex(0x8d9992);
    }
    return owner_face.id >= 0 ? lv_color_hex(0x59ad7a) : lv_color_hex(0xe0a83d);
}

}  // namespace

GuardDisplay::GuardDisplay(Display* display) : display_(display)
{
}

GuardDisplay::~GuardDisplay()
{
    Reset();
}

void GuardDisplay::UpdateObservation(const GuardObservation& observation,
                                     const GuardOwnerFaceDisplay& owner_face)
{
    if (observation.state == GuardState::Disabled) {
        Reset();
        return;
    }
    if (EnsurePanel()) {
        ApplyState(observation, owner_face);
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
    owner_label_ = nullptr;
    owner_detail_label_ = nullptr;
    profile_label_ = nullptr;
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
    lv_label_set_text(title, "LOCAL GUARD");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6ece7), 0);

    state_label_ = lv_label_create(panel_);
    lv_obj_set_width(state_label_, display_->width() - 24);
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 29);
    lv_obj_set_style_text_align(state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(state_label_, ColorForState(GuardState::Idle), 0);

    detail_label_ = lv_label_create(panel_);
    lv_obj_set_width(detail_label_, display_->width() - 24);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 53);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(detail_label_, lv_color_hex(0xd2dbd5), 0);

    motion_bar_ = lv_bar_create(panel_);
    lv_bar_set_range(motion_bar_, 0, 64);
    lv_obj_set_size(motion_bar_, display_->width() - 24, 4);
    lv_obj_align(motion_bar_, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(motion_bar_, lv_color_hex(0x2c332f), LV_PART_MAIN);
    lv_obj_set_style_bg_color(motion_bar_, ColorForState(GuardState::Idle), LV_PART_INDICATOR);
    lv_obj_set_style_radius(motion_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(motion_bar_, 0, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, 0, LV_ANIM_OFF);

    owner_label_ = lv_label_create(panel_);
    lv_obj_set_width(owner_label_, display_->width() - 24);
    lv_obj_align(owner_label_, LV_ALIGN_TOP_MID, 0, 94);
    lv_obj_set_style_text_align(owner_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(owner_label_, lv_color_hex(0x8d9992), 0);

    owner_detail_label_ = lv_label_create(panel_);
    lv_obj_set_width(owner_detail_label_, display_->width() - 24);
    lv_obj_align(owner_detail_label_, LV_ALIGN_TOP_MID, 0, 119);
    lv_obj_set_style_text_align(owner_detail_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(owner_detail_label_, lv_color_hex(0xd2dbd5), 0);

    profile_label_ = lv_label_create(panel_);
    lv_obj_set_width(profile_label_, display_->width() - 24);
    lv_obj_align(profile_label_, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_set_style_text_align(profile_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(profile_label_, lv_color_hex(0x8d9992), 0);

    epoch_label_ = lv_label_create(panel_);
    lv_obj_set_width(epoch_label_, display_->width() - 24);
    lv_obj_align(epoch_label_, LV_ALIGN_TOP_MID, 0, 169);
    lv_obj_set_style_text_align(epoch_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(epoch_label_, lv_color_hex(0x8d9992), 0);
    return true;
}

void GuardDisplay::ApplyState(const GuardObservation& observation,
                              const GuardOwnerFaceDisplay& owner_face)
{
    DisplayLockGuard lock(display_);
    if (state_label_ == nullptr || detail_label_ == nullptr || owner_label_ == nullptr ||
        owner_detail_label_ == nullptr || profile_label_ == nullptr ||
        epoch_label_ == nullptr || motion_bar_ == nullptr) {
        return;
    }

    const lv_color_t color = ColorForState(observation.state);
    lv_label_set_text(state_label_, LabelForState(observation.state, observation.fault));
    lv_obj_set_style_text_color(state_label_, color, 0);
    lv_label_set_text_fmt(detail_label_, "Movement level: %lu",
                          static_cast<unsigned long>(observation.motion_score));
    lv_label_set_text_fmt(epoch_label_, "Activity event: %lu   State change: %lu",
                          static_cast<unsigned long>(observation.epoch),
                          static_cast<unsigned long>(observation.sequence));
    lv_obj_set_style_bg_color(motion_bar_, color, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, std::min<uint32_t>(observation.motion_score, 64), LV_ANIM_OFF);

    const bool result_fresh = owner_face.has_result &&
                              owner_face.result_age_ms <= kOwnerFaceResultFreshMs;
    if (!owner_face.profile_active) {
        lv_label_set_text(owner_label_, "Owner profile not ready");
        lv_label_set_text(owner_detail_label_, "Add owner photos in Admin");
        lv_label_set_text(profile_label_, "No owner profile on this device");
    } else if (!result_fresh) {
        lv_label_set_text(owner_label_, "Looking for owner...");
        lv_label_set_text(owner_detail_label_, "Face check runs every 1.5 seconds");
        lv_label_set_text_fmt(profile_label_, "Profile version: %lu   Photos: %lu",
                              static_cast<unsigned long>(owner_face.profile_revision),
                              static_cast<unsigned long>(owner_face.template_count));
    } else if (owner_face.faces <= 0) {
        lv_label_set_text(owner_label_, "No face detected");
        lv_label_set_text(owner_detail_label_, "Owner match: not available");
        lv_label_set_text_fmt(profile_label_, "Profile version: %lu   Checked: %lus ago",
                              static_cast<unsigned long>(owner_face.profile_revision),
                              static_cast<unsigned long>(owner_face.result_age_ms / 1000));
    } else if (owner_face.id < 0) {
        lv_label_set_text(owner_label_, "Face is not the owner");
        lv_label_set_text_fmt(owner_detail_label_, "Owner match: no match   Faces: %d",
                              owner_face.faces);
        lv_label_set_text_fmt(profile_label_, "Profile version: %lu   Checked: %lus ago",
                              static_cast<unsigned long>(owner_face.profile_revision),
                              static_cast<unsigned long>(owner_face.result_age_ms / 1000));
    } else {
        const int similarity_percent = static_cast<int>(
            std::clamp(owner_face.similarity, 0.0f, 1.0f) * 100.0f + 0.5f);
        lv_label_set_text(owner_label_, "OWNER RECOGNIZED");
        lv_label_set_text_fmt(owner_detail_label_, "Owner match: %d%%   Faces: %d",
                              similarity_percent, owner_face.faces);
        lv_label_set_text_fmt(profile_label_, "Profile version: %lu   Checked: %lus ago",
                              static_cast<unsigned long>(owner_face.profile_revision),
                              static_cast<unsigned long>(owner_face.result_age_ms / 1000));
    }
    lv_obj_set_style_text_color(owner_label_, OwnerColor(owner_face), 0);
}

}  // namespace eidolon
