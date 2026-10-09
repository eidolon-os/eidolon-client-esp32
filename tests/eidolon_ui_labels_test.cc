#include <cassert>
#include <cstring>

#include "eidolon/eidolon_ui_labels.h"
#include "eidolon/ui_state_mapper.h"

namespace {

using namespace eidolon;

EidolonUiModel ProjectWithTouch(const EidolonRuntimeStatus& status) {
    UiInputProfile inputs;
    inputs.enabled_inputs=inputs.available_inputs=InputBit(UiInputSource::Touch);
    return UiStateProjector::Project(status,inputs);
}

void Expect(const char* actual, const char* expected)
{
    assert(actual != nullptr);
    assert(std::strcmp(actual, expected) == 0);
}

EidolonRuntimeStatus ReadyStatus()
{
    EidolonRuntimeStatus status;
    status.runtime = RuntimePhase::Normal;
    status.enrollment = EnrollmentPhase::ClaimActive;
    status.service = ServicePhase::Ready;
    return status;
}

void TestSafetyAndRuntimePrecedence()
{
    auto status = ReadyStatus();
    status.conversation = ConversationPhase::Active;
    status.turn = TurnPhase::AgentSpeaking;

    status.runtime = RuntimePhase::Commissioning;
    auto model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Commissioning);
    assert(!model.primary_enabled);

    status.runtime = RuntimePhase::RecoveryRequired;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::RecoveryRequired);
    assert(model.severity == UiSeverity::Error);

    status.runtime = RuntimePhase::Normal;
    status.enrollment = EnrollmentPhase::Revoked;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Removed);
    assert(!model.show_end_action);
}

void TestOpenSetupIsVisibleWithoutRestoringAdmission()
{
    for (const auto enrollment : {EnrollmentPhase::Unknown,
                                  EnrollmentPhase::PendingReview,
                                  EnrollmentPhase::Revoked}) {
        auto status = ReadyStatus();
        status.enrollment = enrollment;
        status.service = ServicePhase::Unavailable;
        status.runtime = RuntimePhase::Commissioning;
        status.runtime_detail = "Ready for secure device setup";
        status.enrollment_detail = "Previous enrollment is no longer usable";
        auto model = ProjectWithTouch(status);
        assert(model.scene == UiScene::Commissioning);
        Expect(model.detail_text, "Ready for secure device setup");
        assert(!model.primary_enabled);
        assert(!model.show_end_action);
        assert(status.enrollment == enrollment);

        // Closing setup must reveal the underlying recovery/removal verdict.
        status.runtime = RuntimePhase::RecoveryRequired;
        status.runtime_detail = "Waiting for Owner to restore device access";
        model = ProjectWithTouch(status);
        assert(model.scene == (enrollment == EnrollmentPhase::Revoked
                                   ? UiScene::Removed
                                   : UiScene::RecoveryRequired));
        assert(!model.primary_enabled);
    }
}

void TestEnrollmentAndServiceAreOrthogonal()
{
    auto status = ReadyStatus();
    status.enrollment = EnrollmentPhase::PendingReview;
    status.service = ServicePhase::Ready;
    auto model = ProjectWithTouch(status);
    assert(model.scene == UiScene::WaitingApproval);
    Expect(model.detail_text, "Approve this device in Eidolon");

    status.enrollment = EnrollmentPhase::ClaimActive;
    status.service = ServicePhase::Preparing;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::PreparingService);
    Expect(model.detail_text, "Waiting for service configuration");

    status.service_detail = "Waiting for Channel binding";
    model = ProjectWithTouch(status);
    Expect(model.detail_text, "Waiting for Channel binding");
}

void TestStartupDoesNotClaimReadinessOrOwnershipEarly()
{
    EidolonRuntimeStatus status;
    status.runtime = RuntimePhase::Normal;
    for (auto phase : {ServicePhase::Unavailable, ServicePhase::DiscoveringAuthority,
                       ServicePhase::Registering, ServicePhase::Connecting}) {
        status.service = phase;
        const auto model = ProjectWithTouch(status);
        assert(model.scene == UiScene::PreparingService);
        assert(!model.primary_enabled);
        assert(std::strstr(model.detail_text, "claimed") == nullptr);
    }
    status.service = ServicePhase::Fault;
    assert(ProjectWithTouch(status).scene == UiScene::Error);
    status.service = ServicePhase::Unreachable;
    assert(ProjectWithTouch(status).scene == UiScene::Reconnecting);
    status.enrollment = EnrollmentPhase::ClaimActive;
    status.service = ServicePhase::Connecting;
    assert(!UiStateProjector::AllowsIntent(status,UiIntent::OpenConversation));
    status.service = ServicePhase::Ready;
    assert(UiStateProjector::AllowsIntent(status,UiIntent::OpenConversation));
}

void TestRecoverableServiceLossIsNotADeviceError()
{
    auto status = ReadyStatus();
    status.service = ServicePhase::Unreachable;

    auto model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Reconnecting);
    assert(model.severity == UiSeverity::Attention);
    Expect(model.state_label, "RETRY");
    Expect(model.status_text, "Host unavailable");
    assert(!model.end_allowed);
    assert(!model.show_end_action);
    Expect(model.detail_text, "Check Host and Wi-Fi. Retrying automatically.");
    assert(!model.primary_enabled);

    status.conversation = ConversationPhase::Reconnecting;
    model = ProjectWithTouch(status);
    Expect(model.state_label, "REJOIN");
    assert(model.end_allowed);
    Expect(model.status_text, "Reconnecting");
    Expect(model.detail_text, "Connection interrupted. Trying to resume...");

    status.conversation = ConversationPhase::Ended;
    status.end_reason = EndReason::IdleNormalEnd;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Reconnecting);
    assert(!model.primary_enabled);

    status.conversation = ConversationPhase::Closed;
    status.end_reason = EndReason::None;
    status.service = ServicePhase::Fault;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Error);
    assert(model.severity == UiSeverity::Error);
    Expect(model.detail_text, "Service unavailable");
}

void TestConversationRequiresExplicitStart()
{
    auto status = ReadyStatus();
    auto model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Ready);
    assert(model.primary_intent == UiIntent::OpenConversation);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::OpenConversation));

    status.conversation = ConversationPhase::Opening;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::OpeningConversation);
    assert(!model.primary_enabled);
    assert(model.show_end_action);
    assert(!UiStateProjector::AllowsIntent(status, UiIntent::OpenConversation));
    assert(UiStateProjector::AllowsIntent(status, UiIntent::CloseConversation));

    status.conversation = ConversationPhase::Active;
    status.interaction_mode = InteractionMode::PushToTalk;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Conversation);
    Expect(model.mode_label, "PTT");
    assert(model.primary_intent == UiIntent::BeginTalk);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::BeginTalk));

    status.turn = TurnPhase::Recording;
    model = ProjectWithTouch(status);
    assert(model.primary_intent == UiIntent::CommitTalk);
    Expect(model.state_label, "REC");
    Expect(model.detail_text, "Release to send");
    assert(UiStateProjector::AllowsIntent(status, UiIntent::CommitTalk));
}

void TestTurnAndModeProjection()
{
    auto status = ReadyStatus();
    status.conversation = ConversationPhase::Active;
    status.interaction_mode = InteractionMode::FullDuplex;

    status.turn = TurnPhase::UserSpeaking;
    auto model = ProjectWithTouch(status);
    Expect(model.mode_label, "FULL");
    Expect(model.state_label, "LISTEN");
    Expect(model.detail_text, "Listening...");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::ToggleMicrophone));
    assert(!UiStateProjector::AllowsIntent(status, UiIntent::CommitTalk));

    status.turn = TurnPhase::AgentThinking;
    model = ProjectWithTouch(status);
    Expect(model.state_label, "THINK");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);

    status.turn = TurnPhase::AgentSpeaking;
    model = ProjectWithTouch(status);
    Expect(model.state_label, "SPEAK");
    Expect(model.detail_text, "Eidolon is speaking");

    status.last_transcription = "hello";
    status.last_transcription_role = "assistant";
    model = ProjectWithTouch(status);
    Expect(model.subtitle, "hello");
    Expect(model.subtitle_role, "assistant");

    status.interaction_mode = InteractionMode::HalfDuplex;
    status.turn = TurnPhase::UserSpeaking;
    model = ProjectWithTouch(status);
    Expect(model.mode_label, "HALF");
    Expect(model.state_label, "LISTEN");
    Expect(model.detail_text, "Listening...");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::ToggleMicrophone));

    // A stale PTT-only phase must not leak release instructions into an
    // automatic endpointing UI.
    status.turn = TurnPhase::Recording;
    model = ProjectWithTouch(status);
    assert(model.turn == TurnPhase::Idle);
    Expect(model.state_label, "LISTEN");
    Expect(model.detail_text, "Listening...");
}

void TestEndReasonAndDetailOwnership()
{
    auto status = ReadyStatus();
    status.runtime_detail = "stale boot detail";
    status.service_detail = "stale service detail";
    status.conversation = ConversationPhase::Ended;
    status.end_reason = EndReason::Error;
    auto model = ProjectWithTouch(status);
    assert(model.scene == UiScene::Ended);
    assert(model.severity == UiSeverity::Error);
    Expect(model.detail_text, "Could not continue. Ready to try again.");

    status.runtime = RuntimePhase::NetworkConnecting;
    model = ProjectWithTouch(status);
    Expect(model.detail_text, "stale boot detail");
}

void TestPresenceDoesNotOverrideConversation()
{
    auto status = ReadyStatus();
    status.presence_wake = PresenceWakePhase::VerifyingOwner;
    auto model = ProjectWithTouch(status);
    Expect(model.state_label, "VERIFY");

    status.conversation = ConversationPhase::Opening;
    model = ProjectWithTouch(status);
    assert(model.scene == UiScene::OpeningConversation);
    Expect(model.state_label, "OPENING");
}

}  // namespace

int main()
{
    Expect(UiSceneDetail(UiScene::Ended, EndReason::Busy),
           "Device busy. End the current conversation first.");
    Expect(EidolonBrandLabel(), "EIDOLON");
    TestStartupDoesNotClaimReadinessOrOwnershipEarly();
    TestSafetyAndRuntimePrecedence();
    TestOpenSetupIsVisibleWithoutRestoringAdmission();
    TestEnrollmentAndServiceAreOrthogonal();
    TestRecoverableServiceLossIsNotADeviceError();
    TestConversationRequiresExplicitStart();
    TestTurnAndModeProjection();
    TestEndReasonAndDetailOwnership();
    TestPresenceDoesNotOverrideConversation();
    return 0;
}
