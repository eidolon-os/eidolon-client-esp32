#include "eidolon_ui_presenter.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "eidolon_ui_labels.h"
#include "eidolon_view.h"
#include "ui_state_mapper.h"

#include <esp_log.h>

#define TAG "EidolonUiPresenter"

namespace eidolon {

namespace {
// How long to keep showing "processing" after release/commit with no agent
// signal. The observed STT-final + end-of-turn gap before the server reports
// thinking is a few seconds; this is the safety net for the no-response case
// (e.g. empty STT) so the UI doesn't hang on "processing" forever.
#ifndef CONFIG_EIDOLON_PTT_COMMIT_UI_TIMEOUT_MS
#define CONFIG_EIDOLON_PTT_COMMIT_UI_TIMEOUT_MS 5000
#endif

constexpr uint64_t kCommitTimeoutUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_PTT_COMMIT_UI_TIMEOUT_MS) * 1000ULL;
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
            ESP_LOGW(TAG, "PTT commit UI timeout; returning to ready state");
            self->Reapply();
        }
    });
}

DeviceState EidolonUiPresenter::MapToDeviceState(VoiceSessionState session_state,
                                                 AgentPhase phase) const
{
    switch (session_state) {
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
        return kDeviceStateConnecting;
    case VoiceSessionState::InRoom:
        if (phase == AgentPhase::AgentSpeaking) {
            return kDeviceStateSpeaking;
        }
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

void EidolonUiPresenter::SyncDeviceState()
{
    auto device_state = MapToDeviceState(session_state_, tracker_.GetPhase());
    if (app_.GetDeviceState() != device_state) {
        app_.SetDeviceState(device_state);
    }
}

void EidolonUiPresenter::Apply(VoiceSessionState session_state, bool mic_enabled,
                               EndReason end_reason)
{
    ESP_LOGI(TAG, "[EIDOLON_UI] apply session_state=%s end_reason=%d",
             EidolonVoiceController::VoiceStateName(session_state),
             static_cast<int>(end_reason));
    session_state_ = session_state;
    end_reason_ = end_reason;
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

    Reapply();
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

void EidolonUiPresenter::OnPttTurnStatus(const std::string& outcome)
{
    ESP_LOGI(TAG, "PTT turn status received outcome=%s", outcome.c_str());
    if (outcome == "recording" || outcome == "finalizing") {
        return;
    }
    if (outcome == "committed") {
        // The server has accepted the PTT turn; restart the bridge from this
        // stronger signal instead of timing out from the original touch release.
        BeginCommitting();
        Reapply();
        return;
    }
    if (outcome.rfind("rejected:", 0) == 0 ||
        outcome.rfind("cancelled:", 0) == 0) {
        tracker_.OnAgentPhase(AgentPhase::Silent);
    }
    ClearCommitting();
    Reapply();
}

void EidolonUiPresenter::SetLifecyclePhase(LifecyclePhase phase, const std::string& detail)
{
    if (lifecycle_phase_ == phase && lifecycle_detail_ == detail) {
        return;
    }
    const auto old_phase = lifecycle_phase_;
    lifecycle_phase_ = phase;
    lifecycle_detail_ = detail;
    ESP_LOGI(TAG, "[EIDOLON_UI] lifecycle %s -> %s detail=%s",
             LifecycleLabel(old_phase), LifecycleLabel(phase),
             lifecycle_detail_.empty() ? DefaultLifecycleDetail(phase)
                                       : lifecycle_detail_.c_str());
    if (phase == LifecyclePhase::Operational) {
        Reapply();
    } else {
        ApplyLifecycle();
    }
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
    if (lifecycle_phase_ != LifecyclePhase::Operational) {
        ApplyLifecycle();
        return;
    }
    auto snapshot = UiStateMapper::Map(session_state_, tracker_.GetPhase(),
                                       tracker_.LastTranscription(),
                                       tracker_.LastTranscriptionSource(), mic_enabled_,
                                       ptt_recording_, ptt_committing_, {}, end_reason_);
    // Functional input routing must not sit behind a potentially slow display
    // backend. The UI is a projection of session state, not its owner.
    SyncDeviceState();
    ApplySnapshot(snapshot);
}

void EidolonUiPresenter::ApplyLifecycle()
{
    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    const char* state = LifecycleLabel(lifecycle_phase_);
    const char* detail = lifecycle_detail_.empty() ? DefaultLifecycleDetail(lifecycle_phase_)
                                                   : lifecycle_detail_.c_str();
    if (last_visible_state_ != state) {
        ESP_LOGI(TAG, "[EIDOLON_UI] visible state=%s source=lifecycle", state);
        last_visible_state_ = state;
    }
    display->SetEidolonLifecycle(state, detail);
}

void EidolonUiPresenter::ApplySnapshot(const EidolonUiSnapshot& snapshot)
{
    if (auto* view = GetEidolonView()) {
        view->Render(snapshot);
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    const char* state = CompactStateLabel(snapshot);
    if (last_visible_state_ != state) {
        ESP_LOGI(TAG,
                 "[EIDOLON_UI] visible state=%s source=voice pairing=%d connection=%d turn=%d",
                 state, static_cast<int>(snapshot.pairing),
                 static_cast<int>(snapshot.connection), static_cast<int>(snapshot.turn));
        last_visible_state_ = state;
    }
    display->SetVoiceChrome(EidolonBrandLabel(), state, "EXIT",
                            snapshot.connection == ConnectionPhase::InRoom);
    if (snapshot.emotion && snapshot.emotion[0] != '\0') {
        display->SetEmotion(snapshot.emotion);
    }
    if (snapshot.status_text && snapshot.status_text[0] != '\0') {
        display->SetStatus(snapshot.status_text);
    }
    const bool conversation_visible = snapshot.pairing == PairingStatus::Active &&
                                      snapshot.connection == ConnectionPhase::InRoom;
    const bool has_subtitle = conversation_visible && snapshot.subtitle &&
                              snapshot.subtitle[0] != '\0';
    display->SetChatMessage(has_subtitle && snapshot.subtitle_role ? snapshot.subtitle_role
                                                                   : "system",
                            has_subtitle ? snapshot.subtitle : CompactVoiceDetail(snapshot));
}

}  // namespace eidolon
