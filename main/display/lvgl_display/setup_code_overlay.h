#ifndef SETUP_CODE_OVERLAY_H_
#define SETUP_CODE_OVERLAY_H_

#include <lvgl.h>

#include <functional>
#include <string>

#include "setup_code_screen.h"


// The setup code, drawn over whatever view the board shows, on the display's
// top layer. One implementation for every LVGL display, rather than one per
// view: a board whose view never learned about setup codes still shows one.
//
// Light background and dark modules regardless of theme, because a QR code a
// phone cannot read in a dark theme is no code at all. The digits are drawn as
// two groups so a person can type them; the font has digits only.
class SetupCodeOverlay {
public:
    // Both called with the display's LVGL lock held.
    void Show(lv_display_t* display, const SetupCodeScreen& screen);
    void Hide();
    bool visible() const { return root_ != nullptr; }

private:
    static void OnCancel(lv_event_t* event);

    lv_obj_t* root_ = nullptr;
    // Cancel on a touch screen, where the gesture that opened setup (a tap on
    // a view this overlay covers) cannot be made again to close it.
    std::function<void()> cancel_;
    // What is on screen now, so an unchanged card is not rebuilt (and does not
    // flicker) when the window moves from advertising to a connected session.
    std::string shown_qr_text_;
    std::string shown_notice_;
    bool shown_cancel_ = false;
};

#endif  // SETUP_CODE_OVERLAY_H_
