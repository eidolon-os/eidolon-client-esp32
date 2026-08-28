#ifndef EIDOLON_STACKCHAN_AVATAR_VIEW_H_
#define EIDOLON_STACKCHAN_AVATAR_VIEW_H_

#include <lvgl.h>

#include <memory>

#include "eidolon/eidolon_view.h"

class Display;

namespace stackchan::avatar {
class DefaultAvatar;
}

namespace eidolon {

// Per-device voice view for the M5Stack CoreS3 (StackChan body): the full 320x240
// screen is the StackChan expressive avatar (eyes / mouth / speech bubble). The
// presenter's fully-resolved snapshot drives the face:
//   - emotion  <- snapshot.emotion + connection/pairing overrides (the avatar's mood)
//   - speech   <- projected transcript subtitle
//   - mode     <- projected PTT / HALF / FULL label
// A ~20ms LVGL timer runs avatar->update() so decorator/animation lifetimes advance.
// The avatar engine is the ported StackChan renderer (LVGL 9 + smooth_ui_toolkit).
class StackChanAvatarView : public EidolonView {
public:
    struct BuildContext {
        lv_obj_t* parent = nullptr;        // active screen (avatar covers 320x240)
        const lv_font_t* font = nullptr;   // theme text font (CJK-capable) for the bubble
        Display* display = nullptr;        // used only for the LVGL lock guard
    };

    StackChanAvatarView();
    ~StackChanAvatarView() override;

    // Creates the avatar on ctx.parent. The caller must already hold the display
    // lock (called from the board's SetupUI, like the rest of UI setup).
    void Build(const BuildContext& ctx);

    void Render(const EidolonUiModel& model) override;

    // Transiently override the face with a short-lived emotion (e.g. "happy" on
    // owner-presence wake). For ttl_ms the pulse wins over the presenter-resolved
    // emotion; after it expires the face reverts to the resolved mood. Safe to call
    // from any task (just stores state; the LVGL-task timer applies it).
    void PulseEmotion(const char* emotion, int ttl_ms);

private:
    static void OnUpdateTimer(lv_timer_t* timer);
    // Apply the effective emotion (pulse if active, else the resolved base) to the
    // avatar. Must run under the LVGL lock (Render + OnUpdateTimer both hold it).
    void ApplyEmotion();

    Display* display_ = nullptr;
    std::unique_ptr<stackchan::avatar::DefaultAvatar> avatar_;
    lv_obj_t* mode_label_ = nullptr;
    lv_timer_t* update_timer_ = nullptr;
    // Emotion state as ints (the Emotion enum stays out of this header — the board TU
    // only sees a forward-declared DefaultAvatar). Render sets base_; PulseEmotion sets
    // the pulse; ApplyEmotion picks the effective one and de-dupes setEmotion calls.
    int base_emotion_code_ = 0;
    int pulse_emotion_code_ = 0;
    int last_applied_code_ = -1;
    volatile int64_t pulse_until_us_ = 0;
};

}  // namespace eidolon

#endif  // EIDOLON_STACKCHAN_AVATAR_VIEW_H_
