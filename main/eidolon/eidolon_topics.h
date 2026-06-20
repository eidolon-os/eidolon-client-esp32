#ifndef EIDOLON_TOPICS_H_
#define EIDOLON_TOPICS_H_

namespace eidolon {

// Single source of truth for the LiveKit data-channel topics shared between the
// device and the channel agent. Previously these were defined ad-hoc in several
// translation units (kControlTopic was even defined twice), which risked drift.
// Keep in sync with eidolon_channel.
inline constexpr const char* kControlTopic = "eidolon.control";
inline constexpr const char* kClientAudioStateTopic = "eidolon.audio_state";
inline constexpr const char* kUiStateTopic = "eidolon.ui_state";
inline constexpr const char* kSessionControlTopic = "eidolon.session_control";
inline constexpr const char* kTranscriptionTopic = "transcription";

}  // namespace eidolon

#endif  // EIDOLON_TOPICS_H_
