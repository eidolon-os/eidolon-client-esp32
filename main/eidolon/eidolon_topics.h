#ifndef EIDOLON_TOPICS_H_
#define EIDOLON_TOPICS_H_

namespace eidolon {

// C++ mirror of the SDK wire-contract topics shared between device, hub, and
// channel. Previously these were defined ad-hoc in several translation units
// (kControlTopic was even defined twice), which risked drift. Keep in sync with
// eidolon_sdk.biz.contracts.
inline constexpr const char* kControlTopic = "eidolon.control";
inline constexpr const char* kClientAudioStateTopic = "eidolon.audio_state";
inline constexpr const char* kUiStateTopic = "eidolon.ui_state";
inline constexpr const char* kSessionControlTopic = "eidolon.session_control";
inline constexpr const char* kTranscriptionTopic = "lk.transcription";
inline constexpr const char* kAgentSessionTopic = "lk.agent.session";

// ---------------------------------------------------------------------------
// The rest of the device⇄server wire vocabulary. SOURCE OF RECORD is the Python
// module eidolon_sdk/biz/contracts; this header is the hand-kept C++
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
inline constexpr const char* kControlOpPttTurnStatus = "ptt.turn_status";
inline constexpr const char* kControlOpConfigRefresh = "config.refresh";
inline constexpr const char* kControlOpDeviceIdentify = "device.identify";
// Body/head motion (StackChan servo body). Discrete, brain/hub-driven gestures — NOT a
// coordinate stream; continuous gaze tracking is a separate device-local reflex.
inline constexpr const char* kControlOpHeadLookAt = "head.look_at";
inline constexpr const char* kControlOpHeadHome = "head.home";
inline constexpr const char* kControlOpHeadGesture = "head.gesture";
// Emergency stop: cut servo torque immediately (head goes limp) and preempt any running
// gesture. Highest motion priority; safe-risk op any caller may issue.
inline constexpr const char* kControlOpSafetyStop = "safety.stop";
inline constexpr const char* kControlOpDeviceRollCall = "device.roll_call";
inline constexpr const char* kControlOpGuardVisionBenchmark = "guard.vision.benchmark";
inline constexpr const char* kControlOpGuardRuntimeSync = "guard.runtime.sync";
inline constexpr const char* kControlOpGuardOwnerFaceProfileSync =
    "guard.owner_face_profile.sync";

// Guard facts — source of record: eidolon_sdk.biz.guard.protocol.
inline constexpr const char* kGuardPresenceCandidateType = "guard.presence.candidate";
inline constexpr const char* kGuardPresenceAbsentType = "guard.presence.absent";
inline constexpr const char* kGuardOwnerPresenceType = "guard.owner_presence";

// Session metadata enums — declared via X-Device-Interaction-Mode /
// X-Device-Session-Intent headers; hub stamps them into the LiveKit token.
inline constexpr const char* kInteractionModeHalfDuplex = "half_duplex";
inline constexpr const char* kInteractionModeFullDuplex = "full_duplex";
inline constexpr const char* kSessionIntentUserInitiated = "user_initiated";
inline constexpr const char* kSessionIntentProactive = "proactive_initiated";

}  // namespace eidolon

#endif  // EIDOLON_TOPICS_H_
