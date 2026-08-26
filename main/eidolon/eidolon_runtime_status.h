#ifndef EIDOLON_RUNTIME_STATUS_H_
#define EIDOLON_RUNTIME_STATUS_H_

#include <string>

#include "eidolon_flows.h"

namespace eidolon {

// Device-independent facts consumed by the Eidolon experience projection.
// These are orthogonal observations, not one global lifecycle state machine.
enum class RuntimePhase {
    Booting,
    LoadingAssets,
    NetworkScanning,
    NetworkConnecting,
    Commissioning,
    Normal,
    Updating,
    RecoveryRequired,
    Fault,
};

enum class EnrollmentPhase {
    Unknown,
    PendingReview,
    ClaimActive,
    Revoked,
};

enum class ServicePhase {
    Unavailable,
    DiscoveringAuthority,
    Registering,
    Preparing,
    Ready,
    Connecting,
    Reconnecting,
    Unreachable,
    Fault,
};

enum class ConversationPhase {
    Closed,
    Opening,
    Active,
    Reconnecting,
    Ended,
    Failed,
};

enum class AgentPhase {
    Silent,
    UserSpeaking,
    AgentThinking,
    AgentSpeaking,
};

enum class PresenceWakePhase {
    Idle,
    VerifyingOwner,
    OwnerRecognized,
};

enum class EndReason {
    None,
    IdleNormalEnd,
    ProactiveDone,
    UserLeft,
    Superseded,
    Error,
};

struct VoiceRuntimeStatus {
    EnrollmentPhase enrollment = EnrollmentPhase::Unknown;
    ServicePhase service = ServicePhase::Unavailable;
    ConversationPhase conversation = ConversationPhase::Closed;
    EndReason end_reason = EndReason::None;
    bool mic_enabled = true;
};

struct EidolonRuntimeStatus {
    RuntimePhase runtime = RuntimePhase::Booting;
    EnrollmentPhase enrollment = EnrollmentPhase::Unknown;
    ServicePhase service = ServicePhase::Unavailable;
    ConversationPhase conversation = ConversationPhase::Closed;
    TurnPhase turn = TurnPhase::Idle;
    EndReason end_reason = EndReason::None;
    PresenceWakePhase presence_wake = PresenceWakePhase::Idle;
    InteractionMode interaction_mode = CurrentInteractionMode();
    bool mic_enabled = true;
    std::string runtime_detail;
    std::string enrollment_detail;
    std::string service_detail;
    std::string last_transcription;
    const char* last_transcription_role = "system";
};

}  // namespace eidolon

#endif  // EIDOLON_RUNTIME_STATUS_H_
