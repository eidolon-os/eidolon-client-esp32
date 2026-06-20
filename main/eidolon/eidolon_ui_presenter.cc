#include "eidolon_ui_presenter.h"

#include "application.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "display.h"
#include "eidolon_display_hooks.h"
#include "ui_state_mapper.h"

#include <esp_log.h>

#define TAG "EidolonUiPresenter"

namespace eidolon {

EidolonUiPresenter::EidolonUiPresenter(Application& app, Display& display)
    : app_(app), display_(display)
{
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
                                       ptt_recording_);
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
    display_.SetChatMessage(role, event.text.c_str());
    Reapply();
}

void EidolonUiPresenter::OnAgentPhase(AgentPhase phase)
{
    tracker_.OnAgentPhase(phase);
    Reapply();
}

void EidolonUiPresenter::SetPttRecording(bool recording)
{
    if (ptt_recording_ == recording) {
        return;
    }
    ptt_recording_ = recording;
    Reapply();
}

void EidolonUiPresenter::Reapply()
{
    auto snapshot = UiStateMapper::Map(session_state_, tracker_.GetPhase(),
                                       tracker_.LastTranscription(),
                                       tracker_.LastTranscriptionSource(), mic_enabled_,
                                       ptt_recording_);
    ApplySnapshot(snapshot);
}

static const char* ButtonLabel(VoiceSessionButtonState state)
{
    switch (state) {
    case VoiceSessionButtonState::Start:
        return Lang::Strings::ROOM_START;
    case VoiceSessionButtonState::Cancel:
        return Lang::Strings::ROOM_CANCEL;
    case VoiceSessionButtonState::End:
        return Lang::Strings::ROOM_END;
    case VoiceSessionButtonState::Talk:
        // Label is dynamic (hold vs release) and supplied via snapshot.button_label.
        return Lang::Strings::EIDOLON_PTT_HOLD;
    case VoiceSessionButtonState::Hidden:
    default:
        return "";
    }
}

void EidolonUiPresenter::ApplySnapshot(const EidolonUiSnapshot& snapshot)
{
    display_.SetStatus(snapshot.status_text);
    display_.SetEmotion(snapshot.emotion);

    if (snapshot.subtitle != nullptr && snapshot.subtitle[0] != '\0') {
        display_.SetChatMessage(snapshot.subtitle_role, snapshot.subtitle);
    }

    if (snapshot.button_state == VoiceSessionButtonState::Hidden) {
        UpdateVoiceSessionButton(VoiceSessionButtonState::Hidden, "");
    } else {
        const char* label = snapshot.button_label != nullptr ? snapshot.button_label
                                                             : ButtonLabel(snapshot.button_state);
        UpdateVoiceSessionButton(snapshot.button_state, label);
    }
}

}  // namespace eidolon
