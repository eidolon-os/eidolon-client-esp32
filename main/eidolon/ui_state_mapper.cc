#include "ui_state_mapper.h"

#include "assets/lang_config.h"

namespace eidolon {

EidolonUiSnapshot UiStateMapper::Map(VoiceSessionState session_state,
                                     AgentPhase agent_phase,
                                     const std::string& last_transcription,
                                     bool mic_enabled)
{
    EidolonUiSnapshot snapshot;
    snapshot.show_mute_icon = !mic_enabled;

  switch (session_state) {
    case VoiceSessionState::Idle:
    case VoiceSessionState::ConfigReady:
      snapshot.status_text = Lang::Strings::EIDOLON_READY;
      snapshot.button_state = VoiceSessionButtonState::Start;
      snapshot.emotion = "neutral";
      break;
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
      snapshot.status_text = Lang::Strings::ROOM_CONNECTING;
      snapshot.button_state = VoiceSessionButtonState::Cancel;
      snapshot.emotion = "neutral";
      break;
    case VoiceSessionState::InRoom:
      snapshot.status_text = Lang::Strings::ROOM_IN_CALL;
      snapshot.button_state = VoiceSessionButtonState::End;
      snapshot.emotion = "neutral";
      break;
    case VoiceSessionState::Error:
      snapshot.status_text = Lang::Strings::ERROR;
      snapshot.button_state = VoiceSessionButtonState::Start;
      snapshot.emotion = "sad";
      break;
  }

  if (!last_transcription.empty()) {
    snapshot.subtitle = last_transcription.c_str();
  }

  (void)agent_phase;
  return snapshot;
}

}  // namespace eidolon
