#ifndef EIDOLON_AMOLED_206_PTT_VIEW_H_
#define EIDOLON_AMOLED_206_PTT_VIEW_H_

#include <lvgl.h>

#include "eidolon/eidolon_view.h"

class Display;

namespace eidolon {

// Per-device voice view for the Waveshare ESP32-S3-Touch-AMOLED-2.06
// (410x502, touch). The layout keeps one visual language across half-duplex and
// full-duplex devices:
//   - a pressable central voice ring for the turn state and primary action,
//   - an always-visible mode badge (PTT / LIVE), and
//   - a small top close control while a voice room is active.
// Status text and footer/transcript text still delegate to the shared Display.
class Amoled206PttView : public EidolonView {
public:
    struct BuildContext {
        lv_obj_t* parent = nullptr;        // active screen
        const lv_font_t* font = nullptr;   // theme text font (CJK-capable subset)
        lv_color_t accent_color;           // button fill
        Display* display = nullptr;        // status/emotion/transcript sink
        lv_obj_t* legacy_status_label = nullptr; // hidden; this view owns top chrome
        lv_obj_t* legacy_emoji_box = nullptr;  // hidden for this voice-ring layout
    };

    // Creates the eidolon-specific widgets. The caller must already hold the
    // display lock (called from the board's SetupUI, like the rest of UI setup).
    void Build(const BuildContext& ctx);

    void Render(const EidolonUiSnapshot& snapshot) override;
    void ShowChatMessage(const char* role, const char* content) override;

private:
    void SetObjectVisible(lv_obj_t* obj, bool visible);
    void StyleEndButton(lv_obj_t* btn);
    void RenderModeBadge(const EidolonUiSnapshot& snapshot);
    void RenderVoiceRing(const EidolonUiSnapshot& snapshot);
    void RenderControls(const EidolonUiSnapshot& snapshot);
    void RenderFooter(const EidolonUiSnapshot& snapshot);
    void HandleRingEvent(lv_event_t* e);
    static void OnRingEvent(lv_event_t* e);

    Display* display_ = nullptr;
    lv_obj_t* legacy_status_label_ = nullptr;
    lv_obj_t* legacy_emoji_box_ = nullptr;
    lv_color_t accent_color_;
    lv_color_t live_color_;
    lv_color_t idle_color_;
    lv_color_t muted_color_;
    lv_obj_t* end_btn_ = nullptr;
    lv_obj_t* end_label_ = nullptr;
    lv_obj_t* mode_badge_ = nullptr;
    lv_obj_t* mode_badge_label_ = nullptr;
    lv_obj_t* ring_outer_ = nullptr;
    lv_obj_t* ring_inner_ = nullptr;
    lv_obj_t* ring_label_ = nullptr;
    InteractionMode last_mode_ = InteractionMode::PushToTalk;
    ConnectionPhase last_connection_ = ConnectionPhase::Offline;
    bool join_request_pending_ = false;
};

}  // namespace eidolon

#endif  // EIDOLON_AMOLED_206_PTT_VIEW_H_
