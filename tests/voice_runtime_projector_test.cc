#include <cassert>

#include "eidolon/voice_runtime_projector.h"

namespace {

using namespace eidolon;

VoiceRuntimeProjectionInput ActiveClaim(VoiceSessionState state)
{
    VoiceRuntimeProjectionInput input;
    input.session = state;
    input.enrollment = EnrollmentPhase::ClaimActive;
    return input;
}

void TestOperationalReadinessOwnsServiceReadiness()
{
    auto input = ActiveClaim(VoiceSessionState::ConfigReady);
    auto status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Connecting);
    assert(status.conversation == ConversationPhase::Closed);

    input.operational_ready = true;
    status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Ready);

    input.operational_ready = false;
    input.service_was_ready = true;
    status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Reconnecting);
    assert(status.conversation == ConversationPhase::Closed);
}

void TestExhaustedStandbyRecoveryIsStillRecoverable()
{
    auto input = ActiveClaim(VoiceSessionState::ServerUnreachable);
    input.service_was_ready = true;
    auto status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Unreachable);
    assert(status.conversation == ConversationPhase::Closed);

    input.operational_ready = true;
    status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Ready);
    assert(status.conversation == ConversationPhase::Closed);
}

void TestConversationRecoveryStaysNonTerminal()
{
    auto input = ActiveClaim(VoiceSessionState::Reconnecting);
    input.service_was_ready = true;
    auto status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Reconnecting);
    assert(status.conversation == ConversationPhase::Reconnecting);
}

void TestFaultAndRevocationRemainTerminal()
{
    auto input = ActiveClaim(VoiceSessionState::Error);
    auto status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Fault);
    assert(status.conversation == ConversationPhase::Failed);

    input.session = VoiceSessionState::Unauthorized;
    status = VoiceRuntimeProjector::Project(input);
    assert(status.enrollment == EnrollmentPhase::Revoked);
    assert(status.service == ServicePhase::Unavailable);
}

void TestNormalEndSurvivesARecoveringStandbyChannel()
{
    auto input = ActiveClaim(VoiceSessionState::ServerUnreachable);
    input.end_reason = EndReason::IdleNormalEnd;
    auto status = VoiceRuntimeProjector::Project(input);
    assert(status.service == ServicePhase::Unreachable);
    assert(status.conversation == ConversationPhase::Ended);
    assert(status.end_reason == EndReason::IdleNormalEnd);
}

}  // namespace

int main()
{
    TestOperationalReadinessOwnsServiceReadiness();
    TestExhaustedStandbyRecoveryIsStillRecoverable();
    TestConversationRecoveryStaysNonTerminal();
    TestFaultAndRevocationRemainTerminal();
    TestNormalEndSurvivesARecoveringStandbyChannel();
    return 0;
}
