#ifndef EIDOLON_UI_PRESENTER_H_
#define EIDOLON_UI_PRESENTER_H_

#include "agent_session_tracker.h"
#include "device_state.h"
#include "eidolon_ui_types.h"
#include "eidolon_voice_controller.h"

class Application;
class Display;

namespace eidolon {

class EidolonUiPresenter {
public:
    EidolonUiPresenter(Application& app, Display& display);

    void Apply(VoiceSessionState session_state, bool mic_enabled);
    void OnTranscription(const std::string& text);

    AgentSessionTracker& tracker() { return tracker_; }

private:
    void ApplySnapshot(const EidolonUiSnapshot& snapshot);
    DeviceState MapToDeviceState(VoiceSessionState session_state) const;

    Application& app_;
    Display& display_;
    AgentSessionTracker tracker_;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_PRESENTER_H_
