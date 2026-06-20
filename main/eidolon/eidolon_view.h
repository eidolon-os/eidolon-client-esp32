#ifndef EIDOLON_VIEW_H_
#define EIDOLON_VIEW_H_

#include "eidolon_ui_types.h"

namespace eidolon {

// Device-agnostic view interface. The presenter renders a fully-resolved
// EidolonUiSnapshot to whatever per-device view the board registered; it no longer
// pokes Display::Set*/buttons directly. A board selects its view (e.g.
// Amoled206PttView) in display setup. This is the seam for per-device layouts
// (round screens, BOX-3, etc.) without touching the presenter or controller.
class EidolonView {
public:
    virtual ~EidolonView() = default;

    // Project the snapshot onto the screen: status/emotion, mode badge, the
    // talk/start button, and any flow-specific chrome.
    virtual void Render(const EidolonUiSnapshot& snapshot) = 0;

    // Append a chat/transcript line (incremental transcription updates).
    virtual void ShowChatMessage(const char* role, const char* content) = 0;
};

// Registered by the board during display setup; read by the presenter. The board
// owns the view for the lifetime of the display (the registry holds a raw pointer).
void SetEidolonView(EidolonView* view);
EidolonView* GetEidolonView();

}  // namespace eidolon

#endif  // EIDOLON_VIEW_H_
