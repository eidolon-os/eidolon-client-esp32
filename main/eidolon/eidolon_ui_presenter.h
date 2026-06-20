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
    void OnTranscription(const TranscriptionEvent& event);
    void OnAgentPhase(AgentPhase phase);
    // Push-to-talk: the user is currently holding the talk button (mic recording).
    void SetPttRecording(bool recording);

    AgentSessionTracker& tracker() { return tracker_; }

private:
    void Reapply();
    void ApplySnapshot(const EidolonUiSnapshot& snapshot);
    DeviceState MapToDeviceState(VoiceSessionState session_state) const;

    Application& app_;
    Display& display_;
    AgentSessionTracker tracker_;
    VoiceSessionState session_state_ = VoiceSessionState::Idle;
    bool mic_enabled_ = true;
    bool ptt_recording_ = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_PRESENTER_H_
