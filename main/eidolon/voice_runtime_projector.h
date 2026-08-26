#ifndef EIDOLON_VOICE_RUNTIME_PROJECTOR_H_
#define EIDOLON_VOICE_RUNTIME_PROJECTOR_H_

#include "eidolon_runtime_status.h"
#include "voice_session_state.h"

namespace eidolon {

// Transport/controller observations needed to derive the board-independent
// service and conversation lifecycle facts consumed by the UI presenter.
struct VoiceRuntimeProjectionInput {
    VoiceSessionState session = VoiceSessionState::Idle;
    EnrollmentPhase enrollment = EnrollmentPhase::Unknown;
    EndReason end_reason = EndReason::None;
    bool operational_ready = false;
    bool service_was_ready = false;
    bool mic_enabled = true;
};

class VoiceRuntimeProjector {
public:
    static VoiceRuntimeStatus Project(const VoiceRuntimeProjectionInput& input);
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_RUNTIME_PROJECTOR_H_
