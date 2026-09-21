#ifndef EIDOLON_VIEW_H_
#define EIDOLON_VIEW_H_

#include <functional>
#include "expression/surface.h"

#include "eidolon_ui_model.h"
#include "ui_input_profile.h"

namespace eidolon {

// Device-agnostic view interface. The presenter renders a fully-resolved
// EidolonUiModel to whatever per-device view the board registered; it no longer
// pokes Display::Set*/buttons directly. A board selects its view (e.g.
// Amoled206PttView) in display setup. This is the seam for per-device layouts
// (round screens, BOX-3, etc.) without touching the presenter or controller.
class EidolonView {
public:
    virtual ~EidolonView() = default;

    // Project the snapshot onto the screen: status/emotion, mode badge, the
    // talk/start button, and any flow-specific chrome.
    virtual expression::Surface* Expressions() { return nullptr; }
    virtual void Render(const EidolonUiModel& model) = 0;
    virtual void SetMotionDiagnostic(const char*, const char*, const char*) {}

};

// Registered by the board during display setup; read by the presenter. The board
// owns the view for the lifetime of the display (the registry holds a raw pointer).
void SetEidolonView(EidolonView* view);
EidolonView* GetEidolonView();

using UiIntentHandler = std::function<void(UiIntent)>;
void SetEidolonUiIntentHandler(UiIntentHandler handler);
void DispatchEidolonUiIntent(UiIntent intent);
using UiInputHandler = std::function<void(UiInputSource, UiInputGesture)>;
void SetEidolonUiInputHandler(UiInputHandler handler);
void DispatchEidolonUiInput(UiInputSource source, UiInputGesture gesture);
void SetEidolonInputAvailabilityHandler(std::function<void(UiInputSource, bool)> handler);
void SetEidolonInputAvailable(UiInputSource source, bool available);
UiInputProfile CurrentUiInputProfile();
void SetEidolonSetupHandler(std::function<void()> handler);
void OpenEidolonSetup();

}  // namespace eidolon

#endif  // EIDOLON_VIEW_H_
