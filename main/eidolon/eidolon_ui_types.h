#ifndef EIDOLON_UI_TYPES_H_
#define EIDOLON_UI_TYPES_H_

#include <string>

namespace eidolon {

enum class AgentPhase {
    Silent,
    UserSpeaking,
    AgentThinking,
    AgentSpeaking,
};

enum class TranscriptionSource {
    Unknown,
    User,
    Agent,
    System,
};

struct TranscriptionEvent {
    TranscriptionSource source = TranscriptionSource::Unknown;
    std::string text;
    bool is_final = false;
};

enum class VoiceSessionButtonState {
    Hidden,
    Start,
    Cancel,
    End,
    Talk,  // push-to-talk: visible in-room, label set via button_label (hold/release)
};

struct EidolonUiSnapshot {
    const char* status_text = "";
    const char* subtitle = "";
    const char* subtitle_role = "system";
    const char* emotion = "neutral";
    VoiceSessionButtonState button_state = VoiceSessionButtonState::Start;
    // When set, overrides the default label for button_state. Used by the PTT
    // Talk button whose label depends on whether the user is currently holding.
    const char* button_label = nullptr;
    bool show_mute_icon = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_TYPES_H_
