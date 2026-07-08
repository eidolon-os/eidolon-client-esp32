#ifndef EIDOLON_UI_PRESENTER_H_
#define EIDOLON_UI_PRESENTER_H_

#include "agent_session_tracker.h"
#include "device_state.h"
#include "eidolon_ui_types.h"
#include "eidolon_voice_controller.h"

#include <esp_timer.h>

#include <string>

class Application;

namespace eidolon {

class EidolonUiPresenter {
public:
    explicit EidolonUiPresenter(Application& app);
    ~EidolonUiPresenter();

    void Apply(VoiceSessionState session_state, bool mic_enabled,
               EndReason end_reason = EndReason::None);
    void OnTranscription(const TranscriptionEvent& event);
    void OnAgentPhase(AgentPhase phase);
    void OnPttTurnStatus(const std::string& outcome);
    // Push-to-talk: the user is currently holding the talk button (mic recording).
    void SetPttRecording(bool recording);

    AgentSessionTracker& tracker() { return tracker_; }

private:
    void Reapply();
    void ApplySnapshot(const EidolonUiSnapshot& snapshot);
    void SyncDeviceState();
    DeviceState MapToDeviceState(VoiceSessionState session_state, AgentPhase phase) const;
    // Bridge the gap between PTT release and the agent's first thinking/speaking
    // signal: hold a "processing" state so the UI never flashes back to standby.
    void BeginCommitting();
    void ClearCommitting();
    static void CommitTimeoutCb(void* arg);

    Application& app_;
    AgentSessionTracker tracker_;
    VoiceSessionState session_state_ = VoiceSessionState::Idle;
    EndReason end_reason_ = EndReason::None;
    bool mic_enabled_ = true;
    bool ptt_recording_ = false;
    bool ptt_committing_ = false;
    esp_timer_handle_t commit_timer_ = nullptr;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_PRESENTER_H_
