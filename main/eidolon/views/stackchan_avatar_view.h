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
//   - speech   <- incremental transcript lines (ShowChatMessage)
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

    void Render(const EidolonUiSnapshot& snapshot) override;
    void ShowChatMessage(const char* role, const char* content) override;

private:
    static void OnUpdateTimer(lv_timer_t* timer);

    Display* display_ = nullptr;
    std::unique_ptr<stackchan::avatar::DefaultAvatar> avatar_;
    lv_timer_t* update_timer_ = nullptr;
};

}  // namespace eidolon

#endif  // EIDOLON_STACKCHAN_AVATAR_VIEW_H_
