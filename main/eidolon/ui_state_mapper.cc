#include "ui_state_mapper.h"

#include "assets/lang_config.h"

namespace eidolon {

EidolonUiSnapshot UiStateMapper::Map(VoiceSessionState session_state,
                                     AgentPhase agent_phase,
                                     const std::string& last_transcription,
                                     TranscriptionSource last_transcription_source,
                                     bool mic_enabled)
{
    EidolonUiSnapshot snapshot;
    snapshot.show_mute_icon = !mic_enabled;

    switch (session_state) {
    case VoiceSessionState::Idle:
    case VoiceSessionState::ConfigReady:
        snapshot.status_text = Lang::Strings::EIDOLON_READY;
        snapshot.button_state = VoiceSessionButtonState::Start;
        snapshot.emotion = "neutral";
        break;
    case VoiceSessionState::PendingApproval:
        snapshot.status_text = Lang::Strings::EIDOLON_WAITING_APPROVAL;
        snapshot.subtitle = Lang::Strings::EIDOLON_WAITING_APPROVAL_HINT;
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        snapshot.emotion = "neutral";
        break;
    case VoiceSessionState::WaitingBinding:
        snapshot.status_text = Lang::Strings::EIDOLON_WAITING_BINDING;
        snapshot.subtitle = Lang::Strings::EIDOLON_WAITING_BINDING_HINT;
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        snapshot.emotion = "neutral";
        break;
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
        snapshot.status_text = Lang::Strings::ROOM_CONNECTING;
        snapshot.button_state = VoiceSessionButtonState::Cancel;
        snapshot.emotion = "neutral";
        break;
    case VoiceSessionState::InRoom:
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        snapshot.emotion = "neutral";
        switch (agent_phase) {
        case AgentPhase::AgentThinking:
            snapshot.status_text = Lang::Strings::PROCESSING;
            break;
        case AgentPhase::AgentSpeaking:
            snapshot.status_text = Lang::Strings::SPEAKING;
            break;
        case AgentPhase::UserSpeaking:
        case AgentPhase::Silent:
        default:
            snapshot.status_text = Lang::Strings::LISTENING;
            break;
        }
        break;
    case VoiceSessionState::Error:
        snapshot.status_text = Lang::Strings::ERROR;
        snapshot.button_state = VoiceSessionButtonState::Start;
        snapshot.emotion = "sad";
        break;
    case VoiceSessionState::Unauthorized:
        snapshot.status_text = Lang::Strings::EIDOLON_UNAUTHORIZED;
        snapshot.subtitle = Lang::Strings::EIDOLON_UNAUTHORIZED_HINT;
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        snapshot.emotion = "neutral";
        break;
    case VoiceSessionState::ServerUnreachable:
        snapshot.status_text = Lang::Strings::EIDOLON_SERVER_UNREACHABLE;
        snapshot.subtitle = Lang::Strings::EIDOLON_SERVER_UNREACHABLE_HINT;
        snapshot.button_state = VoiceSessionButtonState::Start;
        snapshot.emotion = "sad";
        break;
    }

    if (!last_transcription.empty() && session_state != VoiceSessionState::PendingApproval &&
        session_state != VoiceSessionState::WaitingBinding &&
        session_state != VoiceSessionState::Unauthorized &&
        session_state != VoiceSessionState::ServerUnreachable) {
        snapshot.subtitle = last_transcription.c_str();
        switch (last_transcription_source) {
        case TranscriptionSource::User:
            snapshot.subtitle_role = "user";
            break;
        case TranscriptionSource::Agent:
            snapshot.subtitle_role = "assistant";
            break;
        case TranscriptionSource::System:
            snapshot.subtitle_role = "system";
            break;
        case TranscriptionSource::Unknown:
        default:
            snapshot.subtitle_role = "assistant";
            break;
        }
    }

    return snapshot;
}

}  // namespace eidolon
