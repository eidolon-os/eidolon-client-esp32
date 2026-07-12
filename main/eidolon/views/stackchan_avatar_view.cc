#include "eidolon/views/stackchan_avatar_view.h"

#include <cstring>

#include <esp_log.h>

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
        self->avatar_->update();
    }
}

void StackChanAvatarView::Render(const EidolonUiSnapshot& snapshot)
{
    if (!avatar_) {
        return;
    }
    DisplayLockGuard lock(display_);

    // Expression: start from the presenter-resolved emotion, then let connection /
    // pairing states the emotion string doesn't capture take over (they are the
    // avatar's most legible cues for "resting / working / trouble / needs setup").
    Emotion emotion = MapEmotion(snapshot.emotion);
    if (snapshot.pairing != PairingStatus::Active) {
        emotion = Emotion::Doubt;  // pending approval / unauthorized / waiting binding
    } else {
        switch (snapshot.connection) {
        case ConnectionPhase::Offline:
        case ConnectionPhase::Ready:
            // Idle & not in a voice room -> rest, unless the mapper picked a mood.
            if (emotion == Emotion::Neutral) {
                emotion = Emotion::Sleepy;
            }
            break;
        case ConnectionPhase::Connecting:
        case ConnectionPhase::Reconnecting:
            emotion = Emotion::Doubt;
            break;
        case ConnectionPhase::Unreachable:
        case ConnectionPhase::Error:
            emotion = Emotion::Sad;
            break;
        case ConnectionPhase::InRoom:
            // Live turn: keep the presenter's dialogue emotion (thinking -> Doubt, etc.).
            break;
        }
    }
    avatar_->setEmotion(emotion);

    // The speech bubble mirrors the live conversation (ShowChatMessage). Outside a
    // voice room there is no dialogue, so clear any stale line.
    if (snapshot.connection != ConnectionPhase::InRoom) {
        avatar_->clearSpeech();
    }
}

void StackChanAvatarView::ShowChatMessage(const char* /*role*/, const char* content)
{
    if (!avatar_) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (content != nullptr && content[0] != '\0') {
        avatar_->setSpeech(content);
    } else {
        avatar_->clearSpeech();
    }
}

}  // namespace eidolon
