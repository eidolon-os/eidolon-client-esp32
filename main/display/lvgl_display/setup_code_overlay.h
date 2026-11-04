#ifndef SETUP_CODE_OVERLAY_H_
#define SETUP_CODE_OVERLAY_H_

#include <lvgl.h>

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
    lv_obj_t* root_ = nullptr;
    // What is on screen now, so an unchanged card is not rebuilt (and does not
    // flicker) when the window moves from advertising to a connected session.
    std::string shown_qr_text_;
    std::string shown_notice_;
};

#endif  // SETUP_CODE_OVERLAY_H_
