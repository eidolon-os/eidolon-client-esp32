#pragma once

namespace eidolon {

// Facts the microphone decision is made from. The controller owns every one of
// them; nothing here reads a clock, a board or a transport.
struct CaptureInputs {
    bool ptt_mode = false;
    bool half_duplex_mode = false;
    bool mic_enabled = false;
    bool ptt_active = false;             // held, or in the short release tail
    bool playback_active = false;
    bool audio_outputs_allowed = false;  // the session may speak or play cues
    bool microphone_allowed = false;     // the output gate's input grant
    bool motion_window = false;          // a bounded head motion is running
};

struct CaptureGate {
    bool capture_on = false;
    bool mic_muted = true;
    bool ptt_held = false;
};

// The one place the capture state is decided. Whatever applies it applies it to
// every effector that depends on it (microphone and servo-noise rail together),
// so a path that closes the microphone cannot leave the rail behind.
constexpr CaptureGate DeriveCaptureGate(const CaptureInputs& in)
{
    CaptureGate gate;
    gate.ptt_held = in.ptt_mode && in.ptt_active;
    if (in.ptt_mode) {
        // Push-to-talk: the mic is open while held and during the short release
        // tail. Closed otherwise, so playback is not recorded and the ptt=false
        // edge remains the explicit "I'm done" turn boundary.
        gate.capture_on = in.mic_enabled && in.ptt_active;
        gate.mic_muted = !gate.capture_on;
    } else if (in.half_duplex_mode || !in.audio_outputs_allowed) {
        // Half-duplex: auto open-mic, but CLOSED while the agent is speaking (and
        // the playback hangover). A board without device AEC records its own
        // output otherwise. When the agent is idle the mic is open and the
        // server EOT decides the turn — no barge-in.
        gate.capture_on = in.mic_enabled && !in.playback_active;
        gate.mic_muted = !gate.capture_on;
    } else {
        // Full-duplex: keep capture open during playback so device-side AEC can
        // support barge-in.
        gate.capture_on = in.mic_enabled;
        gate.mic_muted = !in.mic_enabled;
    }
    if (!in.microphone_allowed) {
        gate.capture_on = false;
        gate.mic_muted = true;
        gate.ptt_held = false;
    }
    // Without AEC a moving head is heard; a bounded motion is an output window.
    if (in.motion_window && (in.half_duplex_mode || in.ptt_mode)) {
        gate.capture_on = false;
        gate.mic_muted = true;
    }
    return gate;
}

}  // namespace eidolon
