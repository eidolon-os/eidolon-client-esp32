#include "wwl_button_policy.h"

namespace eidolon {
namespace wwl {

BootAction BootPressDown(BootPolicyState& state, bool in_standby) {
    state.press_in_standby = false;
    state.long_press_consumed = false;

    if (in_standby) {
        state.press_in_standby = true;
        return BootAction::WakeFromStandby;
    }

    return BootAction::TalkPress;
}

BootAction BootRelease(BootPolicyState& state, bool voice_active) {
    if (state.long_press_consumed) {
        return BootAction::SwallowRelease;
    }

    if (state.press_in_standby) {
        if (!voice_active) {
            return BootAction::JoinVoice;
        }
        return BootAction::None;
    }

    return BootAction::TalkRelease;
}

BootAction BootLongPress(BootPolicyState& state, bool /* voice_active */) {
    state.long_press_consumed = true;

    if (state.press_in_standby) {
        return BootAction::ForceSetup;
    }

    // Active-voice and idle long press both go through the existing
    // provisioning/setup gate. The active-voice path no longer calls
    // RequestVoiceLeave — use POWER double-click for that.
    return BootAction::LegacyLongPressSetup;
}

}  // namespace wwl
}  // namespace eidolon