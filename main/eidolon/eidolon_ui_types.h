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

enum class VoiceSessionButtonState {
    Hidden,
    Start,
    Cancel,
    End,
};

struct EidolonUiSnapshot {
    const char* status_text = "";
    const char* subtitle = "";
    const char* emotion = "neutral";
    VoiceSessionButtonState button_state = VoiceSessionButtonState::Start;
    bool show_mute_icon = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_TYPES_H_
