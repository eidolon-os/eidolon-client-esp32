#ifndef EIDOLON_AMOLED_206_PTT_VIEW_H_
#define EIDOLON_AMOLED_206_PTT_VIEW_H_

#include <lvgl.h>

#include "eidolon/eidolon_view.h"

class Display;

namespace eidolon {

// Per-device view for the Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502, touch),
// push-to-talk first:
//   - a large hold-to-talk button placed in the lower-middle (never at the very
//     bottom edge, which is hard to tap), and
//   - an always-visible mode badge (PTT / Stream) so the interaction mode is
//     readable at a glance.
// Status text, emotion and transcript delegate to the shared xiaozhi Display.
class Amoled206PttView : public EidolonView {
public:
    struct BuildContext {
        lv_obj_t* parent = nullptr;        // active screen
        const lv_font_t* font = nullptr;   // theme text font (CJK-capable subset)
        lv_color_t accent_color;           // button fill
        Display* display = nullptr;        // status/emotion/transcript sink
    };

    // Creates the eidolon-specific widgets. The caller must already hold the
    // display lock (called from the board's SetupUI, like the rest of UI setup).
    void Build(const BuildContext& ctx);

    void Render(const EidolonUiSnapshot& snapshot) override;
    void ShowChatMessage(const char* role, const char* content) override;

private:
    Display* display_ = nullptr;
    lv_obj_t* talk_btn_ = nullptr;
    lv_obj_t* talk_label_ = nullptr;
    lv_obj_t* mode_badge_ = nullptr;
    lv_obj_t* mode_badge_label_ = nullptr;
};

}  // namespace eidolon

#endif  // EIDOLON_AMOLED_206_PTT_VIEW_H_
