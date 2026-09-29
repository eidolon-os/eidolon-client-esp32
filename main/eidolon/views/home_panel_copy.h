#ifndef EIDOLON_HOME_PANEL_COPY_H_
#define EIDOLON_HOME_PANEL_COPY_H_

#include <string>

#include "eidolon/eidolon_ui_model.h"

// Chinese wording for the smart home panel's system page. The shared projector
// speaks English for every board; this panel is a Chinese product, so it says
// the same facts in Chinese. Nothing here decides a scene, a severity or an
// action: only how a projected string reads on this screen. A string the table
// does not know (a detail the Hub sent, say) is shown as it came.
//
// Every Chinese character here must also draw before the assets partition's
// full text font is loaded: scripts/eidolon/gen_home_panel_fonts.py puts the
// ones the builtin subset lacks into font_home_text_20.
namespace eidolon::home_copy {

// Title for the system page: the scene's own wording, refined by the projected
// status where one scene has several (PreparingService, Reconnecting).
std::string Title(UiScene scene, const char* status_text);
// Detail line, button labels and input hints.
std::string Text(const char* english);
// Leading "NN%" of an update detail, or -1.
int ProgressPercent(const char* detail);

// The system page's setup button. It opens Wi-Fi setup, as a long press on SET
// does.
inline constexpr const char* kSetupAction = "重新配网";

}  // namespace eidolon::home_copy

#endif  // EIDOLON_HOME_PANEL_COPY_H_
