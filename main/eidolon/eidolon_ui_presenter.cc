#include "eidolon_ui_presenter.h"

#include "application.h"
#include "device_state.h"
#include "eidolon_view.h"
#include "ui_state_mapper.h"

#include <esp_log.h>

#define TAG "EidolonUiPresenter"

namespace eidolon {

namespace {
// How long to keep showing "processing" after release with no agent signal. The
// observed STT-final + end-of-turn gap before the server reports thinking is a few
// seconds; this is the safety net for the no-response case (e.g. empty STT) so the
// UI doesn't hang on "processing" forever.
constexpr uint64_t kCommitTimeoutUs = 12ULL * 1000 * 1000;
}

EidolonUiPresenter::EidolonUiPresenter(Application& app) : app_(app)
{
    esp_timer_create_args_t args = {};
    args.callback = &EidolonUiPresenter::CommitTimeoutCb;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "eidolon_commit";
    esp_timer_create(&args, &commit_timer_);
}

EidolonUiPresenter::~EidolonUiPresenter()
{
    if (commit_timer_ != nullptr) {
        esp_timer_stop(commit_timer_);
        esp_timer_delete(commit_timer_);
    }
}

void EidolonUiPresenter::BeginCommitting()
{
    ptt_committing_ = true;
    if (commit_timer_ != nullptr) {
        esp_timer_stop(commit_timer_);
        esp_timer_start_once(commit_timer_, kCommitTimeoutUs);
    }
}

void EidolonUiPresenter::ClearCommitting()
{
    ptt_committing_ = false;
    if (commit_timer_ != nullptr) {
        esp_timer_stop(commit_timer_);
    }
}

void EidolonUiPresenter::CommitTimeoutCb(void* arg)
{
    auto* self = static_cast<EidolonUiPresenter*>(arg);
    // Runs on the esp_timer task; marshal back to the app task before touching UI.
    self->app_.Schedule([self]() {
        if (self->ptt_committing_) {
            self->ptt_committing_ = false;
            self->Reapply();
        }
    });
}

DeviceState EidolonUiPresenter::MapToDeviceState(VoiceSessionState session_state) const
{
    switch (session_state) {
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
        return kDeviceStateConnecting;
    case VoiceSessionState::InRoom:
        return kDeviceStateListening;
    case VoiceSessionState::PendingApproval:
    case VoiceSessionState::WaitingBinding:
    case VoiceSessionState::ConfigReady:
    case VoiceSessionState::Idle:
    case VoiceSessionState::Error:
    default:
        return kDeviceStateIdle;
    }
}

void EidolonUiPresenter::Apply(VoiceSessionState session_state, bool mic_enabled)
{
    session_state_ = session_state;
    mic_enabled_ = mic_enabled;

    if (session_state == VoiceSessionState::InRoom) {
        tracker_.OnRoomConnected();
    } else if (session_state == VoiceSessionState::PendingApproval ||
               session_state == VoiceSessionState::WaitingBinding ||
               session_state == VoiceSessionState::ConfigReady ||
               session_state == VoiceSessionState::Idle ||
               session_state == VoiceSessionState::Unauthorized ||
               session_state == VoiceSessionState::ServerUnreachable) {
        tracker_.OnRoomDisconnected();
    }

    auto snapshot = UiStateMapper::Map(session_state, tracker_.GetPhase(),
                                       tracker_.LastTranscription(),
                                       tracker_.LastTranscriptionSource(), mic_enabled,
                                       ptt_recording_, ptt_committing_);
    ApplySnapshot(snapshot);

    auto device_state = MapToDeviceState(session_state);
    if (app_.GetDeviceState() != device_state) {
        app_.SetDeviceState(device_state);
    }
}

void EidolonUiPresenter::OnTranscription(const TranscriptionEvent& event)
{
    if (event.source == TranscriptionSource::User && !event.is_final) {
        Reapply();
        return;
    }

    tracker_.OnTranscription(event);
    const char* role = "assistant";
    switch (event.source) {
    case TranscriptionSource::User:
        role = "user";
        break;
    case TranscriptionSource::System:
        role = "system";
        break;
    case TranscriptionSource::Agent:
    case TranscriptionSource::Unknown:
    default:
        role = "assistant";
        break;
    }
    if (auto* view = GetEidolonView()) {
        view->ShowChatMessage(role, event.text.c_str());
    }
    Reapply();
}

void EidolonUiPresenter::OnAgentPhase(AgentPhase phase)
{
    // Real agent progress ends the committing bridge; listening/idle do not (the
    // server reports those during the post-release gap we're covering).
    if (phase == AgentPhase::AgentThinking || phase == AgentPhase::AgentSpeaking) {
        ClearCommitting();
    }
    tracker_.OnAgentPhase(phase);
    Reapply();
}

void EidolonUiPresenter::SetPttRecording(bool recording)
{
    if (ptt_recording_ == recording) {
        return;
    }
    ptt_recording_ = recording;
    if (recording) {
        // New turn starting; drop any pending committing state.
        ClearCommitting();
    } else {
        // Released: the turn is sent — show "processing" until the agent responds.
        BeginCommitting();
    }
    Reapply();
}

void EidolonUiPresenter::Reapply()
{
    auto snapshot = UiStateMapper::Map(session_state_, tracker_.GetPhase(),
                                       tracker_.LastTranscription(),
                                       tracker_.LastTranscriptionSource(), mic_enabled_,
                                       ptt_recording_, ptt_committing_);
    ApplySnapshot(snapshot);
}

void EidolonUiPresenter::ApplySnapshot(const EidolonUiSnapshot& snapshot)
{
    if (auto* view = GetEidolonView()) {
        view->Render(snapshot);
    }
}

}  // namespace eidolon
