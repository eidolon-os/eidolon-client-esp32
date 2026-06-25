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

// ---------------------------------------------------------------------------
// The rest of the device⇄server wire vocabulary. SOURCE OF RECORD is the Python
// module eidolon_sdk/eidolon_sdk/contracts; this header is the hand-kept C++
// mirror (Track A2). eidolon_sdk/tests/contracts/test_contracts.py pins the
// Python values — change one there, change it here too. Keep byte-for-byte.
// ---------------------------------------------------------------------------

// Bump in lockstep with Python WIRE_SCHEMA_VERSION on a breaking envelope change.
inline constexpr int kWireSchemaVersion = 1;

// client.audio_state — body "type" label (NOT the routing topic above) + enums.
inline constexpr const char* kClientAudioStateType = "client.audio_state";
inline constexpr const char* kInputModeAuto = "auto";
inline constexpr const char* kInputModePtt = "ptt";
inline constexpr const char* kInputModeManual = "manual";
inline constexpr const char* kPlaybackStateIdle = "idle";
inline constexpr const char* kPlaybackStateAgentSpeaking = "agent_speaking";

// session_end — server → device teardown notice (kSessionControlTopic).
inline constexpr const char* kSessionEndType = "session_end";
inline constexpr const char* kSessionEndIdleNormal = "idle_normal_end";
inline constexpr const char* kSessionEndUserLeft = "user_left";
inline constexpr const char* kSessionEndError = "error";
inline constexpr const char* kSessionEndProactiveDone = "proactive_done";
inline constexpr const char* kSessionEndSuperseded = "superseded";

// Device control ops — carried as "op" in the eidolon.control envelope.
inline constexpr const char* kControlOpRoomJoin = "room.join";
inline constexpr const char* kControlOpPlaybackStop = "playback.stop";
inline constexpr const char* kControlOpConfigRefresh = "config.refresh";

// Session metadata enums — declared via X-Device-Interaction-Mode /
// X-Device-Session-Intent headers; hub stamps them into the LiveKit token.
inline constexpr const char* kInteractionModeHalfDuplex = "half_duplex";
inline constexpr const char* kInteractionModeFullDuplex = "full_duplex";
inline constexpr const char* kSessionIntentUserInitiated = "user_initiated";
inline constexpr const char* kSessionIntentProactive = "proactive_initiated";

}  // namespace eidolon

#endif  // EIDOLON_TOPICS_H_
