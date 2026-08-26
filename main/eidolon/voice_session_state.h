#ifndef EIDOLON_VOICE_SESSION_STATE_H_
#define EIDOLON_VOICE_SESSION_STATE_H_

namespace eidolon {

// Internal, board-independent controller state. UI code must consume the
// orthogonal runtime projection instead of interpreting this state directly.
enum class VoiceSessionState {
    Idle,
    PendingApproval,
    WaitingBinding,
    ConfigReady,
    Connecting,
    Opening,
    InRoom,
    Reconnecting,
    Error,
    Unauthorized,       // revoked or unregistered by admin
    ServerUnreachable,  // standby service unreachable; recovery still continues
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_SESSION_STATE_H_
