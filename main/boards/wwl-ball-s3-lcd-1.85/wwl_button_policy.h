#ifndef WWL_BUTTON_POLICY_H_
#define WWL_BUTTON_POLICY_H_

namespace eidolon {
namespace wwl {

// --- WWL BOOT button policy (pure, testable) ---

// The BOOT button serves two roles that are separated by where the press started:
// from power-latch standby it is a wake source; from an active voice session it
// is PTT; from idle (and non-standby) it is a session control with a long-press
// setup gesture (the existing HubSetupButtonLongPressOpensSetup gate).

// Action the policy prescribes for a single button event.
enum class BootAction {
    None,
    WakeFromStandby,      // press-down while in standby -> wake
    JoinVoice,            // short-press release from standby, no active session
    TalkPress,            // normal press-down during active voice -> PTT press
    TalkRelease,          // normal release during active voice -> PTT release
    ForceSetup,           // long press from standby -> wake + open provision
    LegacyLongPressSetup, // long press from any non-standby state -> existing gate
    SwallowRelease,       // release after long press was consumed
};

// Tracked transient state that the board class owns and feeds into the policy.
// All fields are the caller's responsibility to set before calling the policy.
struct BootPolicyState {
    // Was the button down while in power-latch standby?
    bool press_in_standby = false;
    // Has a long press been detected and consumed during this press cycle?
    bool long_press_consumed = false;
};

// Policy decisions for the BOOT button. Callers provide the event, the
// session-active query result, and the DeviceState for the existing
// HubSetupButtonLongPressOpensSetup gate.
//
// All inputs are passed explicitly: the board owns the policy state struct
// and resets it after each full press/release cycle.

BootAction BootPressDown(BootPolicyState& state, bool in_standby);

BootAction BootRelease(BootPolicyState& state, bool voice_active);

BootAction BootLongPress(BootPolicyState& state, bool voice_active);

}  // namespace wwl
}  // namespace eidolon

#endif  // WWL_BUTTON_POLICY_H_