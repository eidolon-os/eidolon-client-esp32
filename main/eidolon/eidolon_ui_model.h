#ifndef EIDOLON_UI_MODEL_H_
#define EIDOLON_UI_MODEL_H_

#include "eidolon_runtime_status.h"

namespace eidolon {

enum class UiScene {
    Starting,
    Loading,
    Network,
    Commissioning,
    Updating,
    WaitingApproval,
    PreparingService,
    Ready,
    OpeningConversation,
    Conversation,
    Reconnecting,
    Ended,
    Removed,
    RecoveryRequired,
    Error,
};

enum class UiIntent {
    None,
    OpenConversation,
    CloseConversation,
    BeginTalk,
    CommitTalk,
    ToggleMicrophone,
};

enum class UiSeverity {
    Normal,
    Attention,
    Error,
};

// Fully resolved semantic presentation. Board views choose layout and animation;
// they do not reinterpret lifecycle facts or decide which actions are legal.
struct EidolonUiModel {
    UiScene scene = UiScene::Starting;
    UiSeverity severity = UiSeverity::Normal;
    InteractionMode interaction_mode = InteractionMode::Streaming;
    TurnPhase turn = TurnPhase::Idle;
    EndReason end_reason = EndReason::None;
    const char* state_label = "BOOT";
    const char* status_text = "";
    const char* detail_text = "";
    const char* subtitle = "";
    const char* subtitle_role = "system";
    const char* emotion = "neutral";

    UiIntent primary_intent = UiIntent::None;
    const char* primary_label = "";
    bool primary_enabled = false;
    bool show_end_action = false;
    bool show_mute_icon = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_MODEL_H_
