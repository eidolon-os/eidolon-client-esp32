#ifndef EIDOLON_UI_PRESENTER_H_
#define EIDOLON_UI_PRESENTER_H_

#include <cstdint>
#include <string>

#include "agent_session_tracker.h"
#include "eidolon_runtime_status.h"
#include "eidolon_ui_model.h"
#include "eidolon_ui_types.h"

class Application;

namespace eidolon {

class EidolonUiPresenter {
public:
    explicit EidolonUiPresenter(Application& app);
    ~EidolonUiPresenter();

    void ApplyVoiceStatus(const VoiceRuntimeStatus& status);
    void SetRuntimePhase(RuntimePhase phase, const std::string& detail = "");
    void SetEnrollmentPhase(EnrollmentPhase phase, const std::string& detail = "");
    void SetServicePhase(ServicePhase phase, const std::string& detail = "");
    void OnTranscription(const TranscriptionEvent& event);
    void OnAgentPhase(AgentPhase phase);
    void OnPttTurnStatus(const std::string& outcome);
    void OnPresenceWakePhase(PresenceWakePhase phase);
    void SetPttRecording(bool recording);
    // Once a second on the Application task: a start or end the runtime has not
    // answered in time is taken down, and the screen returns to its report.
    void Tick();

    const EidolonRuntimeStatus& runtime_status() const { return status_; }
    AgentSessionTracker& tracker() { return tracker_; }

private:
    void Reapply();
    void ApplyModel(const EidolonUiModel& model);
    void SyncLegacyDeviceState();
    void HandleIntent(UiIntent intent);
    bool BeginSessionRequest(UiIntent intent);
    void SettleSessionRequest();

    Application& app_;
    AgentSessionTracker tracker_;
    EidolonRuntimeStatus status_;
    std::string last_visible_state_;
    uint64_t session_request_ms_ = 0;          // when the request now drawn was made
    UiIntent last_request_ = UiIntent::None;   // the last start or end asked for
    uint64_t last_request_ms_ = 0;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_PRESENTER_H_
