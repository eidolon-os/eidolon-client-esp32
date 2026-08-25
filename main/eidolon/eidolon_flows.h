#ifndef EIDOLON_FLOWS_H_
#define EIDOLON_FLOWS_H_

// Device-independent conversation interaction vocabulary. Runtime, enrollment,
// service and conversation lifecycle facts live in eidolon_runtime_status.h;
// this dependency-light header holds the turn/mode concepts they share.

namespace eidolon {

// Flow D — push-to-talk (half-duplex) vs auto open-mic (full-duplex). Compile-time
// per board via Kconfig; carried as a first-class snapshot field so the UI can show
// it persistently (requirement: mode indicator always visible).
enum class InteractionMode {
    PushToTalk,
    Streaming,
};

// Conversation turn / dialogue intent.
enum class TurnPhase {
    Idle,           // waiting for the user
    UserSpeaking,   // streaming mode: server reports the user is speaking
    Recording,      // push-to-talk: the button is held, mic open
    Committing,     // PTT released, turn sent — awaiting the agent (STT/EOT gap)
    AgentThinking,
    AgentSpeaking,
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
