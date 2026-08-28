#ifndef EIDOLON_FLOWS_H_
#define EIDOLON_FLOWS_H_

// Device-independent conversation interaction vocabulary. Runtime, enrollment,
// service and conversation lifecycle facts live in eidolon_runtime_status.h;
// this dependency-light header holds the turn/mode concepts they share.

namespace eidolon {

// Device interaction mode. This mirrors the three-value wire contract exactly:
// PTT has an explicit release boundary, while half/full duplex both use automatic
// endpointing and differ in whether playback can be interrupted.
enum class InteractionMode {
    PushToTalk,
    HalfDuplex,
    FullDuplex,
};

inline constexpr bool IsPushToTalk(InteractionMode mode)
{
    return mode == InteractionMode::PushToTalk;
}

inline constexpr bool IsAutomaticEndpointing(InteractionMode mode)
{
    return mode == InteractionMode::HalfDuplex ||
           mode == InteractionMode::FullDuplex;
}

// Conversation turn / dialogue intent.
enum class TurnPhase {
    Idle,           // waiting for the user
    UserSpeaking,   // streaming mode: server reports the user is speaking
    Recording,      // push-to-talk: the button is held, mic open
    Committing,     // PTT released, turn sent — awaiting the agent (STT/EOT gap)
    AgentThinking,
    AgentSpeaking,
};

inline constexpr TurnPhase NormalizeTurnPhase(InteractionMode mode, TurnPhase phase)
{
    if (IsPushToTalk(mode)) {
        // PTT recording/finalization is driven by the explicit client-control
        // lifecycle. A streaming user-speaking packet is not authoritative.
        return phase == TurnPhase::UserSpeaking ? TurnPhase::Idle : phase;
    }
    // Automatic endpointing has no local press/release lifecycle. Ignore any
    // stale PTT-only state instead of projecting impossible controls or copy.
    return phase == TurnPhase::Recording || phase == TurnPhase::Committing
               ? TurnPhase::Idle
               : phase;
}

inline InteractionMode CurrentInteractionMode()
{
#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
    return InteractionMode::PushToTalk;
#elif CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX
    return InteractionMode::HalfDuplex;
#else
    return InteractionMode::FullDuplex;
#endif
}

}  // namespace eidolon

#endif  // EIDOLON_FLOWS_H_
