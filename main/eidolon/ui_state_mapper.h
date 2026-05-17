#ifndef EIDOLON_UI_STATE_MAPPER_H_
#define EIDOLON_UI_STATE_MAPPER_H_

#include "eidolon_ui_types.h"
#include "eidolon_voice_controller.h"

namespace eidolon {

class UiStateMapper {
public:
    static EidolonUiSnapshot Map(VoiceSessionState session_state,
                                 AgentPhase agent_phase,
                                 const std::string& last_transcription,
                                 bool mic_enabled);
};

}  // namespace eidolon

#endif  // EIDOLON_UI_STATE_MAPPER_H_
