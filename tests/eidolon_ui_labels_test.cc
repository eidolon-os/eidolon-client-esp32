#include <cassert>
#include <cstring>

#include "eidolon/eidolon_ui_labels.h"
#include "eidolon/ui_state_mapper.h"

namespace {

using namespace eidolon;

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
    auto model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Commissioning);
    assert(!model.primary_enabled);

    status.runtime = RuntimePhase::RecoveryRequired;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::RecoveryRequired);
    assert(model.severity == UiSeverity::Error);

    status.runtime = RuntimePhase::Normal;
    status.enrollment = EnrollmentPhase::Revoked;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Removed);
    assert(!model.show_end_action);
}

void TestEnrollmentAndServiceAreOrthogonal()
{
    auto status = ReadyStatus();
    status.enrollment = EnrollmentPhase::PendingReview;
    status.service = ServicePhase::Ready;
    auto model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::WaitingApproval);
    Expect(model.detail_text, "Approve this device in Eidolon");

    status.enrollment = EnrollmentPhase::ClaimActive;
    status.service = ServicePhase::Preparing;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::PreparingService);
    Expect(model.detail_text, "Device claimed; service is not ready");

    status.service_detail = "Waiting for Channel binding";
    model = UiStateProjector::Project(status);
    Expect(model.detail_text, "Waiting for Channel binding");
}

void TestRecoverableServiceLossIsNotADeviceError()
{
    auto status = ReadyStatus();
    status.service = ServicePhase::Unreachable;

    auto model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Reconnecting);
    assert(model.severity == UiSeverity::Attention);
    Expect(model.state_label, "RETRY");
    Expect(model.status_text, "Restoring service");
    Expect(model.detail_text, "Retrying Channel connection...");
    assert(!model.primary_enabled);

    status.conversation = ConversationPhase::Reconnecting;
    model = UiStateProjector::Project(status);
    Expect(model.state_label, "REJOIN");
    Expect(model.status_text, "Reconnecting");
    Expect(model.detail_text, "Restoring Channel connection...");

    status.conversation = ConversationPhase::Ended;
    status.end_reason = EndReason::IdleNormalEnd;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Reconnecting);
    assert(!model.primary_enabled);

    status.conversation = ConversationPhase::Closed;
    status.end_reason = EndReason::None;
    status.service = ServicePhase::Fault;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Error);
    assert(model.severity == UiSeverity::Error);
    Expect(model.detail_text, "Service unavailable");
}

void TestConversationRequiresExplicitStart()
{
    auto status = ReadyStatus();
    auto model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Ready);
    assert(model.primary_intent == UiIntent::OpenConversation);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::OpenConversation));

    status.conversation = ConversationPhase::Opening;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::OpeningConversation);
    assert(!model.primary_enabled);
    assert(model.show_end_action);
    assert(!UiStateProjector::AllowsIntent(status, UiIntent::OpenConversation));
    assert(UiStateProjector::AllowsIntent(status, UiIntent::CloseConversation));

    status.conversation = ConversationPhase::Active;
    status.interaction_mode = InteractionMode::PushToTalk;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Conversation);
    Expect(model.mode_label, "PTT");
    assert(model.primary_intent == UiIntent::BeginTalk);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::BeginTalk));

    status.turn = TurnPhase::Recording;
    model = UiStateProjector::Project(status);
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
    auto model = UiStateProjector::Project(status);
    Expect(model.mode_label, "FULL");
    Expect(model.state_label, "LISTEN");
    Expect(model.detail_text, "Listening...");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::ToggleMicrophone));
    assert(!UiStateProjector::AllowsIntent(status, UiIntent::CommitTalk));

    status.turn = TurnPhase::AgentThinking;
    model = UiStateProjector::Project(status);
    Expect(model.state_label, "THINK");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);

    status.turn = TurnPhase::AgentSpeaking;
    model = UiStateProjector::Project(status);
    Expect(model.state_label, "SPEAK");
    Expect(model.detail_text, "Eidolon is speaking");

    status.last_transcription = "hello";
    status.last_transcription_role = "assistant";
    model = UiStateProjector::Project(status);
    Expect(model.subtitle, "hello");
    Expect(model.subtitle_role, "assistant");

    status.interaction_mode = InteractionMode::HalfDuplex;
    status.turn = TurnPhase::UserSpeaking;
    model = UiStateProjector::Project(status);
    Expect(model.mode_label, "HALF");
    Expect(model.state_label, "LISTEN");
    Expect(model.detail_text, "Listening...");
    assert(model.primary_intent == UiIntent::ToggleMicrophone);
    assert(UiStateProjector::AllowsIntent(status, UiIntent::ToggleMicrophone));

    // A stale PTT-only phase must not leak release instructions into an
    // automatic endpointing UI.
    status.turn = TurnPhase::Recording;
    model = UiStateProjector::Project(status);
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
    auto model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::Ended);
    assert(model.severity == UiSeverity::Error);
    Expect(model.detail_text, "Conversation ended with an error");

    status.runtime = RuntimePhase::NetworkConnecting;
    model = UiStateProjector::Project(status);
    Expect(model.detail_text, "stale boot detail");
}

void TestPresenceDoesNotOverrideConversation()
{
    auto status = ReadyStatus();
    status.presence_wake = PresenceWakePhase::VerifyingOwner;
    auto model = UiStateProjector::Project(status);
    Expect(model.state_label, "VERIFY");

    status.conversation = ConversationPhase::Opening;
    model = UiStateProjector::Project(status);
    assert(model.scene == UiScene::OpeningConversation);
    Expect(model.state_label, "OPENING");
}

}  // namespace

int main()
{
    Expect(EidolonBrandLabel(), "EIDOLON");
    TestSafetyAndRuntimePrecedence();
    TestEnrollmentAndServiceAreOrthogonal();
    TestRecoverableServiceLossIsNotADeviceError();
    TestConversationRequiresExplicitStart();
    TestTurnAndModeProjection();
    TestEndReasonAndDetailOwnership();
    TestPresenceDoesNotOverrideConversation();
    return 0;
}
