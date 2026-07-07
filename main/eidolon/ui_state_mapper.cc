#include "ui_state_mapper.h"

#include "assets/lang_config.h"

namespace eidolon {

namespace {

// VoiceSessionState bundles three orthogonal concerns; split it back into the
// explicit flows so the rendering below reasons about one thing at a time.

PairingStatus PairingStatusFor(VoiceSessionState state)
{
    switch (state) {
    case VoiceSessionState::PendingApproval:
        return PairingStatus::PendingApproval;
    case VoiceSessionState::WaitingBinding:
        return PairingStatus::WaitingBinding;
    case VoiceSessionState::Unauthorized:
        return PairingStatus::Unauthorized;
    default:
        return PairingStatus::Active;
    }
}

ConnectionPhase ConnectionPhaseFor(VoiceSessionState state)
{
    switch (state) {
    case VoiceSessionState::Connecting:
        return ConnectionPhase::Connecting;
    case VoiceSessionState::Reconnecting:
        return ConnectionPhase::Reconnecting;
    case VoiceSessionState::InRoom:
        return ConnectionPhase::InRoom;
    case VoiceSessionState::ServerUnreachable:
        return ConnectionPhase::Unreachable;
    case VoiceSessionState::Error:
        return ConnectionPhase::Error;
    case VoiceSessionState::ConfigReady:
        return ConnectionPhase::Ready;
    case VoiceSessionState::Idle:
        return ConnectionPhase::Offline;
    // Pairing states are not yet connected; the pairing screen takes precedence
    // so their connection phase is not rendered.
    case VoiceSessionState::PendingApproval:
    case VoiceSessionState::WaitingBinding:
    case VoiceSessionState::Unauthorized:
    default:
        return ConnectionPhase::Ready;
    }
}

TurnPhase TurnPhaseFor(AgentPhase agent_phase, bool ptt_recording, bool ptt_committing)
{
    if (ptt_recording) {
        return TurnPhase::Recording;
    }
    // Real agent progress wins over the optimistic committing bridge.
    if (agent_phase == AgentPhase::AgentThinking) {
        return TurnPhase::AgentThinking;
    }
    if (agent_phase == AgentPhase::AgentSpeaking) {
        return TurnPhase::AgentSpeaking;
    }
    // After release, hold "processing" across the STT/end-of-turn gap (the server
    // still reports listening/idle here) so the UI never flashes back to standby.
    if (ptt_committing) {
        return TurnPhase::Committing;
    }
    if (agent_phase == AgentPhase::UserSpeaking) {
        return TurnPhase::UserSpeaking;
    }
    return TurnPhase::Idle;
}

// The conversation/ready screen: pairing is Active and the connection is settled
// (Offline/Ready/InRoom). Driven by mode + turn so push-to-talk and streaming each
// read straight off the explicit flows.
void RenderConversation(EidolonUiSnapshot& snapshot)
{
    if (snapshot.mode == InteractionMode::PushToTalk) {
        // Two explicit phases: until connected the button is a "connect" tap (the
        // room/agent come up first, then the user holds to talk — so the first
        // words are never dropped). Hold-to-talk only appears in-room.
        if (snapshot.connection != ConnectionPhase::InRoom) {
            snapshot.status_text = Lang::Strings::EIDOLON_READY;
            snapshot.button_state = VoiceSessionButtonState::Start;
            // "连接" reads as "establish the room connection" — clearer than
            // "开始对话", which sounds like you can already talk.
            snapshot.button_label = Lang::Strings::EIDOLON_CONNECT;
            snapshot.emotion = "neutral";
            return;
        }
        // The talk button stays visible whether idle or in-room; hold to record,
        // release to send. Its label flips with whether the user is holding.
        snapshot.button_state = VoiceSessionButtonState::Talk;
        // The button label follows the gesture in context: release-to-send while
        // recording, hold-to-interrupt while the agent is talking, hold-to-talk
        // otherwise.
        if (snapshot.turn == TurnPhase::Recording) {
            snapshot.button_label = Lang::Strings::EIDOLON_PTT_RELEASE;
        } else if (snapshot.turn == TurnPhase::AgentSpeaking) {
            snapshot.button_label = Lang::Strings::ROOM_INTERRUPT;
        } else {
            snapshot.button_label = Lang::Strings::EIDOLON_PTT_HOLD;
        }
        switch (snapshot.turn) {
        case TurnPhase::Recording:
            snapshot.status_text = Lang::Strings::EIDOLON_PTT_RECORDING;
            snapshot.emotion = "happy";
            break;
        case TurnPhase::Committing:
        case TurnPhase::AgentThinking:
            snapshot.status_text = Lang::Strings::EIDOLON_THINKING;
            snapshot.emotion = "thinking";
            break;
        case TurnPhase::AgentSpeaking:
            snapshot.status_text = Lang::Strings::SPEAKING;
            snapshot.emotion = "happy";
            break;
        case TurnPhase::Idle:
        case TurnPhase::UserSpeaking:
        default:
            snapshot.status_text = Lang::Strings::EIDOLON_READY;
            snapshot.emotion = "neutral";
            break;
        }
        return;
    }

    // Streaming (full-duplex): a tap starts the session from the ready screen; once
    // in-room the mic is open continuously and the button is hidden.
    snapshot.emotion = "neutral";
    if (snapshot.connection == ConnectionPhase::InRoom) {
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        switch (snapshot.turn) {
        case TurnPhase::AgentThinking:
            snapshot.status_text = Lang::Strings::EIDOLON_THINKING;
            break;
        case TurnPhase::AgentSpeaking:
            snapshot.status_text = Lang::Strings::SPEAKING;
            break;
        default:
            snapshot.status_text = Lang::Strings::LISTENING;
            break;
        }
    } else {
        snapshot.status_text = Lang::Strings::EIDOLON_READY;
        snapshot.button_state = VoiceSessionButtonState::Start;
    }
}

}  // namespace

EidolonUiSnapshot UiStateMapper::Map(VoiceSessionState session_state,
                                     AgentPhase agent_phase,
                                     const std::string& last_transcription,
                                     TranscriptionSource last_transcription_source,
                                     bool mic_enabled,
                                     bool ptt_recording,
                                     bool ptt_committing,
                                     VoiceInputPolicy input_policy,
                                     EndReason end_reason)
{
    EidolonUiSnapshot snapshot;
    snapshot.show_mute_icon = !mic_enabled;
    snapshot.input_policy = input_policy;
    snapshot.mode = CurrentInteractionMode();
    snapshot.pairing = PairingStatusFor(session_state);
    snapshot.connection = ConnectionPhaseFor(session_state);
    snapshot.turn = TurnPhaseFor(agent_phase, ptt_recording, ptt_committing);

    if (snapshot.pairing != PairingStatus::Active) {
        snapshot.button_state = VoiceSessionButtonState::Hidden;
        snapshot.emotion = "neutral";
        switch (snapshot.pairing) {
        case PairingStatus::PendingApproval:
            snapshot.status_text = Lang::Strings::EIDOLON_WAITING_APPROVAL;
            snapshot.subtitle = Lang::Strings::EIDOLON_WAITING_APPROVAL_HINT;
            break;
        case PairingStatus::WaitingBinding:
            snapshot.status_text = Lang::Strings::EIDOLON_WAITING_BINDING;
            snapshot.subtitle = Lang::Strings::EIDOLON_WAITING_BINDING_HINT;
            break;
        case PairingStatus::Unauthorized:
        default:
            snapshot.status_text = Lang::Strings::EIDOLON_UNAUTHORIZED;
            snapshot.subtitle = Lang::Strings::EIDOLON_UNAUTHORIZED_HINT;
            break;
        }
        return snapshot;
    }

    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
        snapshot.status_text = Lang::Strings::ROOM_CONNECTING;
        snapshot.button_state = VoiceSessionButtonState::Cancel;
        snapshot.emotion = "neutral";
        break;
    case ConnectionPhase::Reconnecting:
        snapshot.status_text = Lang::Strings::EIDOLON_RECONNECTING;
        snapshot.button_state = VoiceSessionButtonState::Cancel;
        snapshot.emotion = "neutral";
        break;
    case ConnectionPhase::Unreachable:
        snapshot.status_text = Lang::Strings::EIDOLON_SERVER_UNREACHABLE;
        snapshot.subtitle = Lang::Strings::EIDOLON_SERVER_UNREACHABLE_HINT;
        snapshot.button_state = VoiceSessionButtonState::Start;
        snapshot.emotion = "sad";
        return snapshot;  // no transcript subtitle while unreachable
    case ConnectionPhase::Error:
        snapshot.status_text = Lang::Strings::ERROR;
        snapshot.button_state = VoiceSessionButtonState::Start;
        snapshot.emotion = "sad";
        break;
    case ConnectionPhase::Offline:
    case ConnectionPhase::Ready:
    case ConnectionPhase::InRoom:
    default:
        RenderConversation(snapshot);
        break;
    }

    // End-of-session overlay (plan §3.2): when we have settled back on the ready
    // screen (control room up, not in a voice room) after the channel told us the
    // conversation ended, say so — instead of a bare "待命" that reads like a
    // never-started JOIN. The CONNECT/Talk button stays so the user can re-engage;
    // the device is still reachable on the control room even after an error.
    // Superseded is intentionally silent: a newer session is already taking over.
    if (snapshot.connection == ConnectionPhase::Ready &&
        snapshot.pairing == PairingStatus::Active) {
        switch (end_reason) {
        case EndReason::IdleNormalEnd:
        case EndReason::ProactiveDone:
        case EndReason::UserLeft:
            snapshot.status_text = Lang::Strings::EIDOLON_SESSION_ENDED;
            snapshot.emotion = "neutral";
            break;
        case EndReason::Error:
            snapshot.status_text = Lang::Strings::EIDOLON_SESSION_ENDED;
            snapshot.emotion = "sad";
            break;
        case EndReason::Superseded:
        case EndReason::None:
        default:
            break;
        }
    }

    if (!last_transcription.empty()) {
        snapshot.subtitle = last_transcription.c_str();
        switch (last_transcription_source) {
        case TranscriptionSource::User:
            snapshot.subtitle_role = "user";
            break;
        case TranscriptionSource::System:
            snapshot.subtitle_role = "system";
            break;
        case TranscriptionSource::Agent:
        case TranscriptionSource::Unknown:
        default:
            snapshot.subtitle_role = "assistant";
            break;
        }
    }

    return snapshot;
}

}  // namespace eidolon
