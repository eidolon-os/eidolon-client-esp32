#ifndef EIDOLON_UI_PRESENTER_H_
#define EIDOLON_UI_PRESENTER_H_

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

    const EidolonRuntimeStatus& runtime_status() const { return status_; }
    AgentSessionTracker& tracker() { return tracker_; }

private:
    void Reapply();
    void ApplyModel(const EidolonUiModel& model);
    void SyncLegacyDeviceState();
    void HandleIntent(UiIntent intent);

    Application& app_;
    AgentSessionTracker tracker_;
    EidolonRuntimeStatus status_;
    std::string last_visible_state_;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_PRESENTER_H_
