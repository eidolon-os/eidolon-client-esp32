#include "eidolon/views/stackchan_avatar_view.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

#include "display.h"

#include "eidolon/avatar/skins/default/default.h"

namespace eidolon {

using stackchan::avatar::DefaultAvatar;
using stackchan::avatar::Emotion;

namespace {
constexpr const char* kTag = "StackChanAvatar";

// The presenter emits a small emotion vocabulary (ui_state_mapper.cc):
// neutral / happy / thinking / sad. Map onto StackChan's 6-value Emotion enum.
Emotion MapEmotion(const char* emotion)
{
    if (emotion == nullptr) {
        return Emotion::Neutral;
    }
    if (std::strcmp(emotion, "happy") == 0) {
        return Emotion::Happy;
    }
    if (std::strcmp(emotion, "sad") == 0) {
        return Emotion::Sad;
    }
    if (std::strcmp(emotion, "thinking") == 0) {
        return Emotion::Doubt;
    }
    if (std::strcmp(emotion, "angry") == 0) {
        return Emotion::Angry;
    }
    if (std::strcmp(emotion, "sleepy") == 0) {
        return Emotion::Sleepy;
    }
    return Emotion::Neutral;
}
}  // namespace

// Out-of-line ctor/dtor: the unique_ptr<DefaultAvatar> deleter needs the complete
// type, which only this TU has (the board TU sees a forward declaration).
StackChanAvatarView::StackChanAvatarView() = default;

StackChanAvatarView::~StackChanAvatarView()
{
    if (update_timer_ != nullptr) {
        lv_timer_delete(update_timer_);
        update_timer_ = nullptr;
    }
}

void StackChanAvatarView::Build(const BuildContext& ctx)
{
    display_ = ctx.display;
    avatar_ = std::make_unique<DefaultAvatar>();
    // The StackChan default skin is a 320x240 canvas — exactly the CoreS3 screen.
    avatar_->init(ctx.parent, ctx.font);
    avatar_->setEmotion(Emotion::Neutral);
    base_emotion_code_ = static_cast<int>(Emotion::Neutral);
    last_applied_code_ = static_cast<int>(Emotion::Neutral);

    // This custom full-screen view bypasses the generic presenter chrome, so it
    // owns a small mode overlay. The text itself is resolved by the shared
    // projector; the board view only renders it.
    mode_label_ = lv_label_create(ctx.parent);
    if (ctx.font != nullptr) {
        lv_obj_set_style_text_font(mode_label_, ctx.font, 0);
    }
    lv_obj_set_style_text_color(mode_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(mode_label_, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(mode_label_, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(mode_label_, 8, 0);
    lv_obj_set_style_pad_ver(mode_label_, 3, 0);
    lv_obj_set_style_radius(mode_label_, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(mode_label_, LV_ALIGN_TOP_MID, 0, 5);
    lv_label_set_text(mode_label_, "");

    // Advance decorator/animation lifetimes. lv_timer callbacks run inside the
    // esp_lvgl_port task (which already holds the LVGL lock), so update() here
    // needs no extra guard.
    update_timer_ = lv_timer_create(&StackChanAvatarView::OnUpdateTimer, 20, this);
    ESP_LOGI(kTag, "StackChan avatar view built");
}

void StackChanAvatarView::OnUpdateTimer(lv_timer_t* timer)
{
    auto* self = static_cast<StackChanAvatarView*>(lv_timer_get_user_data(timer));
    if (self != nullptr && self->avatar_) {
        self->ApplyEmotion();  // honor a pulse window / revert when it expires
        self->avatar_->update();
    }
}

void StackChanAvatarView::ApplyEmotion()
{
    if (!avatar_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    const int code = (now < pulse_until_us_) ? pulse_emotion_code_ : base_emotion_code_;
    if (code != last_applied_code_) {
        avatar_->setEmotion(static_cast<Emotion>(code));
        last_applied_code_ = code;
    }
}

void StackChanAvatarView::PulseEmotion(const char* emotion, int ttl_ms)
{
    if (ttl_ms <= 0) {
        return;
    }
    pulse_emotion_code_ = static_cast<int>(MapEmotion(emotion));
    pulse_until_us_ = esp_timer_get_time() + static_cast<int64_t>(ttl_ms) * 1000;
}

void StackChanAvatarView::Render(const EidolonUiModel& model)
{
    if (!avatar_) {
        return;
    }
    DisplayLockGuard lock(display_);

    // The device-independent projection owns semantic mood. This view only maps
    // that mood onto the avatar engine's visual vocabulary.
    Emotion emotion = MapEmotion(model.emotion);
    if (model.scene == UiScene::Ready && emotion == Emotion::Neutral) {
        emotion = Emotion::Sleepy;
    }
    // Record the resolved mood as the base; a live PulseEmotion window overrides it.
    // ApplyEmotion (here + on the 20ms timer) is the single place that calls setEmotion.
    base_emotion_code_ = static_cast<int>(emotion);
    ApplyEmotion();

    if (mode_label_ != nullptr) {
        lv_label_set_text(mode_label_, model.mode_label);
    }

    if (model.scene == UiScene::Conversation && model.subtitle != nullptr &&
        model.subtitle[0] != '\0') {
        avatar_->setSpeech(model.subtitle);
    } else if (model.detail_text != nullptr && model.detail_text[0] != '\0' &&
               model.scene != UiScene::Conversation) {
        avatar_->setSpeech(model.detail_text);
    } else {
        avatar_->clearSpeech();
    }
}

}  // namespace eidolon
