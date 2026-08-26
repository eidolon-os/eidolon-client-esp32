#include "voice_runtime_projector.h"

namespace eidolon {

namespace {

ConversationPhase RestingConversation(const VoiceRuntimeProjectionInput& input)
{
    return input.end_reason == EndReason::None ? ConversationPhase::Closed
                                               : ConversationPhase::Ended;
}

ServicePhase ObservedService(const VoiceRuntimeProjectionInput& input,
                             ServicePhase unavailable_phase)
{
    return input.operational_ready ? ServicePhase::Ready : unavailable_phase;
}

}  // namespace

VoiceRuntimeStatus VoiceRuntimeProjector::Project(
    const VoiceRuntimeProjectionInput& input)
{
    VoiceRuntimeStatus status;
    status.enrollment = input.enrollment;
    status.end_reason = input.end_reason;
    status.mic_enabled = input.mic_enabled;

    switch (input.session) {
    case VoiceSessionState::Idle:
        status.enrollment = EnrollmentPhase::Unknown;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::PendingApproval:
        status.enrollment = EnrollmentPhase::PendingReview;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::WaitingBinding:
        status.service = ServicePhase::Preparing;
        break;
    case VoiceSessionState::ConfigReady:
        status.service = ObservedService(
            input, input.service_was_ready ? ServicePhase::Reconnecting
                                           : ServicePhase::Connecting);
        status.conversation = RestingConversation(input);
        break;
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Opening:
        status.service = ObservedService(input, ServicePhase::Connecting);
        status.conversation = ConversationPhase::Opening;
        break;
    case VoiceSessionState::InRoom:
        status.service = ServicePhase::Ready;
        status.conversation = ConversationPhase::Active;
        break;
    case VoiceSessionState::Reconnecting:
        status.service = ObservedService(input, ServicePhase::Reconnecting);
        status.conversation = ConversationPhase::Reconnecting;
        break;
    case VoiceSessionState::ServerUnreachable:
        // Repeated failures are a degraded but still self-healing service fact,
        // not a terminal conversation failure. A desired conversation remains
        // in Reconnecting rather than entering this standby-only state.
        status.service = ObservedService(input, ServicePhase::Unreachable);
        status.conversation = RestingConversation(input);
        break;
    case VoiceSessionState::Unauthorized:
        status.enrollment = EnrollmentPhase::Revoked;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::Error:
    default:
        status.service = ServicePhase::Fault;
        status.conversation = ConversationPhase::Failed;
        break;
    }
    return status;
}

}  // namespace eidolon
