#ifndef EIDOLON_FLOWS_H_
#define EIDOLON_FLOWS_H_

// Explicit models for the four interaction flows that used to be tangled inside
// the single flat VoiceSessionState enum. Keeping them orthogonal lets the UI be
// a pure projection (mode badge / connection status / turn intent) instead of one
// switch that mixes pairing, connection, and dialogue concerns.
//
//   A (startup/activation) lives in the xiaozhi Application/Ota DeviceState; its
//     tail (pairing/authorization) surfaces here as PairingStatus.
//   B (room connection)    -> ConnectionPhase
//   C (conversation turn)  -> TurnPhase
//   D (interaction mode)   -> InteractionMode  (cross-cutting, per board)
//
// This header is intentionally dependency-light (no controller include) so it can
// sit under eidolon_ui_types.h without a cycle. The VoiceSessionState -> flow
// derivations live in ui_state_mapper.cc where the controller header is available.

namespace eidolon {

// Flow D — push-to-talk (half-duplex) vs auto open-mic (full-duplex). Compile-time
// per board via Kconfig; carried as a first-class snapshot field so the UI can show
// it persistently (requirement: mode indicator always visible).
enum class InteractionMode {
    PushToTalk,
    Streaming,
};

// Flow A tail — pairing/authorization status. A property of the Hub config, not of
// the live connection.
enum class PairingStatus {
    Active,
    PendingApproval,
    WaitingBinding,
    Unauthorized,
};

// Flow B — channel connection lifecycle. One channel: these phases say what
// the device is doing on it, not which of two rooms it is standing in.
enum class ConnectionPhase {
    Offline,       // no usable config yet / not connected
    Ready,         // active config, on the channel in standby, not in a conversation
    Connecting,    // joining a conversation
    InRoom,        // conversation connected
    Reconnecting,  // transient drop, retrying
    Unreachable,   // repeated failures, rediscovery exhausted
    Error,
};

// Flow C — conversation turn / dialogue intent.
enum class TurnPhase {
    Idle,           // waiting for the user
    UserSpeaking,   // streaming mode: server reports the user is speaking
    Recording,      // push-to-talk: the button is held, mic open
    Committing,     // PTT released, turn sent — awaiting the agent (STT/EOT gap)
    AgentThinking,
    AgentSpeaking,
};

// Cross-cutting input capability. Barge-in is a capability of LIVE/full-duplex
// input, not a third user-facing mode: the top chrome still says LIVE. The default
// is None; boards/controllers should only advance this when capture, AEC, and the
// server path actually support interruption.
enum class InterruptPhase {
    None,
    Available,
    Interrupting,
};

inline InteractionMode CurrentInteractionMode()
{
#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
    return InteractionMode::PushToTalk;
#else
    return InteractionMode::Streaming;
#endif
}

}  // namespace eidolon

#endif  // EIDOLON_FLOWS_H_
