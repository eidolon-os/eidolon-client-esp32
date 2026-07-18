#include "guard/guard_display.h"

#include <algorithm>

#include "display.h"
#include "display/lcd_display.h"

LV_FONT_DECLARE(font_puhui_basic_20_4);

namespace eidolon {
namespace {

constexpr uint64_t kOwnerFaceResultFreshMs = 4000;

enum class OwnerVisualState {
    NoProfile,
    Scanning,
    NoFace,
    Unknown,
    Owner,
    OwnerPresent,
};

OwnerVisualState VisualStateFor(const GuardOwnerFaceDisplay& owner_face)
{
    if (!owner_face.profile_active) {
        return OwnerVisualState::NoProfile;
    }
    if (owner_face.identity_session_active &&
        (owner_face.presence_state == OwnerPresenceState::Present ||
         owner_face.presence_state == OwnerPresenceState::AbsentPending)) {
        return OwnerVisualState::OwnerPresent;
    }
    if (!owner_face.has_result || owner_face.result_age_ms > kOwnerFaceResultFreshMs) {
        return OwnerVisualState::Scanning;
    }
    if (owner_face.faces <= 0) {
        return OwnerVisualState::NoFace;
    }
    return owner_face.id >= 0 ? OwnerVisualState::Owner : OwnerVisualState::Unknown;
}

lv_color_t MotionColor(GuardState state)
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

lv_color_t OwnerColor(OwnerVisualState state)
{
    switch (state) {
    case OwnerVisualState::Owner:
    case OwnerVisualState::OwnerPresent:
        return lv_color_hex(0x59ad7a);
    case OwnerVisualState::Unknown:
    case OwnerVisualState::NoProfile:
        return lv_color_hex(0xe0a83d);
    case OwnerVisualState::Scanning:
        return lv_color_hex(0x5c9db5);
    case OwnerVisualState::NoFace:
        return lv_color_hex(0x8d9992);
    }
    return lv_color_hex(0x8d9992);
}

const char* PresenceExperienceName(OwnerPresenceState state)
{
    switch (state) {
    case OwnerPresenceState::Unavailable: return "Profile unavailable";
    case OwnerPresenceState::Watching: return "Watching for owner";
    case OwnerPresenceState::PresentPending: return "Checking owner";
    case OwnerPresenceState::Present: return "Owner is here";
    case OwnerPresenceState::AbsentPending: return "Owner may be away";
    }
    return "Watching";
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
    owner_label_ = nullptr;
    owner_detail_label_ = nullptr;
    profile_label_ = nullptr;
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
    lv_label_set_text(title, "OWNER GUARD");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6ece7), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    owner_label_ = lv_label_create(panel_);
    lv_obj_set_width(owner_label_, display_->width() - 24);
    lv_obj_align(owner_label_, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_align(owner_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(owner_label_, lv_color_hex(0x8d9992), 0);
    lv_obj_set_style_text_font(owner_label_, &font_puhui_basic_20_4, 0);

    owner_detail_label_ = lv_label_create(panel_);
    lv_obj_set_width(owner_detail_label_, display_->width() - 24);
    lv_obj_align(owner_detail_label_, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_text_align(owner_detail_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(owner_detail_label_, lv_color_hex(0xd2dbd5), 0);
    lv_obj_set_style_text_font(owner_detail_label_, &font_puhui_basic_20_4, 0);

    profile_label_ = lv_label_create(panel_);
    lv_obj_set_width(profile_label_, display_->width() - 24);
    lv_obj_align(profile_label_, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_text_align(profile_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(profile_label_, lv_color_hex(0x8d9992), 0);
    lv_obj_set_style_text_font(profile_label_, &lv_font_montserrat_14, 0);

    motion_bar_ = lv_bar_create(panel_);
    lv_bar_set_range(motion_bar_, 0, 64);
    lv_obj_set_size(motion_bar_, display_->width() - 32, 8);
    lv_obj_align(motion_bar_, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_color(motion_bar_, lv_color_hex(0x2c332f), LV_PART_MAIN);
    lv_obj_set_style_bg_color(motion_bar_, MotionColor(GuardState::Idle), LV_PART_INDICATOR);
    lv_obj_set_style_radius(motion_bar_, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(motion_bar_, 4, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, 0, LV_ANIM_OFF);

    state_label_ = lv_label_create(panel_);
    lv_obj_set_width(state_label_, display_->width() - 24);
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 181);
    lv_obj_set_style_text_align(state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(state_label_, MotionColor(GuardState::Idle), 0);
    lv_obj_set_style_text_font(state_label_, &lv_font_montserrat_14, 0);
    return true;
}

void GuardDisplay::ApplyState(const GuardObservation& observation,
                              const GuardOwnerFaceDisplay& owner_face)
{
    DisplayLockGuard lock(display_);
    if (state_label_ == nullptr || owner_label_ == nullptr ||
        owner_detail_label_ == nullptr || profile_label_ == nullptr ||
        motion_bar_ == nullptr) {
        return;
    }

    const lv_color_t motion_color = MotionColor(observation.state);
    lv_label_set_text_fmt(state_label_, "%s  |  Motion %lu",
                          PresenceExperienceName(owner_face.presence_state),
                          static_cast<unsigned long>(observation.motion_score));
    lv_obj_set_style_text_color(state_label_, motion_color, 0);
    lv_obj_set_style_bg_color(motion_bar_, motion_color, LV_PART_INDICATOR);
    lv_bar_set_value(motion_bar_, std::min<uint32_t>(observation.motion_score, 64), LV_ANIM_OFF);

    const OwnerVisualState owner_state = VisualStateFor(owner_face);
    const int similarity_percent = static_cast<int>(
        std::clamp(owner_face.similarity, 0.0f, 1.0f) * 100.0f + 0.5f);
    switch (owner_state) {
    case OwnerVisualState::NoProfile:
        lv_label_set_text(owner_label_, "NO PROFILE");
        lv_label_set_text(owner_detail_label_, "");
        lv_label_set_text(profile_label_, "Admin sync required");
        break;
    case OwnerVisualState::Scanning:
        lv_label_set_text(owner_label_, "SCANNING");
        lv_label_set_text(owner_detail_label_, "");
        lv_label_set_text_fmt(profile_label_, "Profile %lu  |  %lu photos",
                              static_cast<unsigned long>(owner_face.profile_revision),
                              static_cast<unsigned long>(owner_face.template_count));
        break;
    case OwnerVisualState::NoFace:
        lv_label_set_text(owner_label_, "NO FACE");
        lv_label_set_text(owner_detail_label_, "");
        lv_label_set_text_fmt(profile_label_, "Profile %lu",
                              static_cast<unsigned long>(owner_face.profile_revision));
        break;
    case OwnerVisualState::Unknown:
        lv_label_set_text(owner_label_, "UNKNOWN");
        lv_label_set_text_fmt(owner_detail_label_, "%d%% MATCH", similarity_percent);
        lv_label_set_text_fmt(profile_label_, "Threshold: 50%%  |  Profile %lu",
                              static_cast<unsigned long>(owner_face.profile_revision));
        break;
    case OwnerVisualState::Owner:
        lv_label_set_text(owner_label_, "OWNER");
        lv_label_set_text_fmt(owner_detail_label_, "%d%% MATCH", similarity_percent);
        lv_label_set_text_fmt(profile_label_, "Threshold: 50%%  |  Profile %lu",
                              static_cast<unsigned long>(owner_face.profile_revision));
        break;
    case OwnerVisualState::OwnerPresent: {
        const int presence_percent = static_cast<int>(
            std::clamp(owner_face.person_score, 0.0f, 1.0f) * 100.0f + 0.5f);
        lv_label_set_text(owner_label_, "OWNER");
        if (owner_face.person_evaluated && owner_face.person_present) {
            lv_label_set_text_fmt(owner_detail_label_, "PRESENCE %d%%", presence_percent);
        } else {
            lv_label_set_text(owner_detail_label_, "PRESENCE ACTIVE");
        }
        lv_label_set_text_fmt(profile_label_, "Face-gated session  |  Profile %lu",
                              static_cast<unsigned long>(owner_face.profile_revision));
        break;
    }
    }
    const lv_color_t owner_color = OwnerColor(owner_state);
    lv_obj_set_style_text_color(owner_label_, owner_color, 0);
    lv_obj_set_style_text_color(owner_detail_label_, owner_color, 0);
}

}  // namespace eidolon
