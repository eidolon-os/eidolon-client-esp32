#include "eidolon/capture_gate.h"
#include <cassert>
#include <cstdio>
using namespace eidolon;
namespace {
CaptureInputs Listening(bool ptt, bool half_duplex) {
    CaptureInputs in;
    in.ptt_mode=ptt;in.half_duplex_mode=half_duplex;
    in.mic_enabled=true;in.ptt_active=ptt;
    in.audio_outputs_allowed=true;in.microphone_allowed=true;
    return in;
}
bool Open(const CaptureGate& gate) { return gate.capture_on && !gate.mic_muted; }
bool Closed(const CaptureGate& gate) { return !gate.capture_on && gate.mic_muted; }
}
int main() {
    // Half duplex (StackChan): open while the agent is idle, closed while it
    // speaks or the head moves, closed whenever the gate withholds the mic.
    auto half=Listening(false,true);
    assert(Open(DeriveCaptureGate(half)));
    auto speaking=half;speaking.playback_active=true;
    assert(Closed(DeriveCaptureGate(speaking)));
    auto moving=half;moving.motion_window=true;
    assert(Closed(DeriveCaptureGate(moving)));
    auto muted=half;muted.mic_enabled=false;
    assert(Closed(DeriveCaptureGate(muted)));
    // Conversation over: the suspended gate withholds the mic, so the same
    // derivation that opened capture closes it — and the rail follows.
    auto ended=half;ended.microphone_allowed=false;
    assert(Closed(DeriveCaptureGate(ended)));
    // Push-to-talk: only while held; release and motion close it.
    auto ptt=Listening(true,false);
    auto held=DeriveCaptureGate(ptt);
    assert(Open(held) && held.ptt_held);
    auto released=ptt;released.ptt_active=false;
    assert(Closed(DeriveCaptureGate(released)) && !DeriveCaptureGate(released).ptt_held);
    auto ptt_moving=ptt;ptt_moving.motion_window=true;
    assert(Closed(DeriveCaptureGate(ptt_moving)) && DeriveCaptureGate(ptt_moving).ptt_held);
    auto ptt_ended=ptt;ptt_ended.microphone_allowed=false;
    assert(Closed(DeriveCaptureGate(ptt_ended)) && !DeriveCaptureGate(ptt_ended).ptt_held);
    // Full duplex keeps capture through playback and motion (device AEC), but
    // falls back to half-duplex turn taking when the session may not speak.
    auto full=Listening(false,false);
    full.playback_active=true;full.motion_window=true;
    assert(Open(DeriveCaptureGate(full)));
    auto silent=full;silent.audio_outputs_allowed=false;
    assert(Closed(DeriveCaptureGate(silent)));
    auto full_ended=full;full_ended.microphone_allowed=false;
    assert(Closed(DeriveCaptureGate(full_ended)));
    // A full-duplex user mute reports muted even though nothing else closed it.
    auto full_muted=Listening(false,false);full_muted.mic_enabled=false;
    assert(Closed(DeriveCaptureGate(full_muted)));
    puts("PASS: capture gate across ptt, half duplex, full duplex, playback, motion, mute and closed grant");
}
