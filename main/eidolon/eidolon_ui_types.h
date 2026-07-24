#ifndef EIDOLON_UI_TYPES_H_
#define EIDOLON_UI_TYPES_H_

#include <string>

#include "eidolon_flows.h"

namespace eidolon {

// Device lifecycle before (and around) a voice session. This is deliberately
// orthogonal to pairing/connection/turn: Wi-Fi or Hub recovery may temporarily
// take visual precedence without corrupting the underlying voice state.
enum class LifecyclePhase {
    Booting,
    LoadingAssets,
    WifiScanning,
    WifiConnecting,
    WifiSetup,
    HubDiscovering,
    HubRegistering,
    Updating,
    Operational,
    Offline,
    Error,
};

enum class AgentPhase {
    Silent,
    UserSpeaking,
    AgentThinking,
    AgentSpeaking,
};

enum class TranscriptionSource {
    Unknown,
    User,
    Agent,
    System,
};

// Why a voice session ended, as reported by the channel's session_end{reason}
// packet (plan §3.2). Orthogonal to ConnectionPhase: it lets the UI tell a
// normal end of conversation ("已结束待命") apart from a JOIN failure ("Error"),
// instead of inferring intent from a bare LiveKit ROOM_DELETED. Reserved values
// (ProactiveDone/Superseded) are wired for Phase 3 proactive.
enum class EndReason {
    None,           // no end pending / fresh session
    IdleNormalEnd,  // server idle watchdog — conversation ended normally
    ProactiveDone,  // proactive report finished (Phase 3)
    UserLeft,       // the other side left / job shut down
    Superseded,     // replaced by a newer session (silent switch)
    Error,          // server tore the session down on an error
};

struct TranscriptionEvent {
    TranscriptionSource source = TranscriptionSource::Unknown;
    std::string text;
    bool is_final = false;
};

struct VoiceInputPolicy {
    bool barge_in_enabled = false;
    InterruptPhase interrupt = InterruptPhase::None;
};

enum class VoiceSessionButtonState {
    Hidden,
    Start,
    Cancel,
    End,
    Talk,  // push-to-talk: visible in-room, label set via button_label (hold/release)
};

struct EidolonUiSnapshot {
    // Explicit flow projection (the four decoupled flows). The view renders the
    // mode badge / connection status / turn intent from these; the const char*
    // fields below are the resolved text/emotion for the current view.
    InteractionMode mode = InteractionMode::Streaming;
    PairingStatus pairing = PairingStatus::Active;
    ConnectionPhase connection = ConnectionPhase::Offline;
    TurnPhase turn = TurnPhase::Idle;
    EndReason end_reason = EndReason::None;

    const char* status_text = "";
    const char* subtitle = "";
    const char* subtitle_role = "system";
    const char* emotion = "neutral";
    VoiceSessionButtonState button_state = VoiceSessionButtonState::Start;
    // When set, overrides the default label for button_state. Used by the PTT
    // Talk button whose label depends on whether the user is currently holding.
    const char* button_label = nullptr;
    bool show_mute_icon = false;
    VoiceInputPolicy input_policy;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_TYPES_H_
