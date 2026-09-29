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
    OpenSetup,
};

enum class UiActionPresentation { Hidden, TouchControl, InputHint };

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
    InteractionMode interaction_mode = CurrentInteractionMode();
    TurnPhase turn = TurnPhase::Idle;
    EndReason end_reason = EndReason::None;
    const char* state_label = "BOOT";
    // Resolved by UiStateMapper::Project. Views consume this label directly so
    // board-specific renderers never need to reinterpret the interaction mode.
    const char* mode_label = "";
    const char* status_text = "";
    const char* detail_text = "";
    const char* subtitle = "";
    const char* subtitle_role = "system";
    const char* emotion = "neutral";

    UiIntent primary_intent = UiIntent::None;
    // What a session control (the session button, or a view's on-screen one)
    // does now; see UiStateProjector::SessionControlIntent.
    UiIntent session_intent = UiIntent::None;
    const char* primary_label = "";
    bool primary_enabled = false;
    // Semantic availability is distinct from a control being visible.
    bool end_allowed = false;
    bool show_end_action = false;
    UiActionPresentation primary_presentation = UiActionPresentation::Hidden;
    const char* input_hint = "";
    bool show_mute_icon = false;
    bool show_setup_action = false;
    bool touch_navigation = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_MODEL_H_
