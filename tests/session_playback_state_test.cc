#include "eidolon/session_playback_state.h"
#include <cassert>

using namespace eidolon;

int main()
{
    SessionPlaybackState playback;
    auto observe = [&](const char* state, const char* reason, int64_t now) {
        if (auto signal = SessionPlaybackSignal(state, reason)) {
            playback.SetSpeaking(*signal, now);
        }
    };
    observe("listening", "session_started", 100);
    assert(!playback.Active(100));
    // Reproduce the failing sequence: output on B, VAD/UI events from A.
    observe("speaking", "agent_state:speaking", 200);
    assert(playback.Active(200));
    observe("listening", "user_state:speaking", 300);
    observe("listening", "user_state:listening", 400);
    observe("idle", "user_state:away", 500);
    assert(playback.Active(500));
    // A has no local playback clock. Its gate remains closed through the tail.
    observe("listening", "agent_state:listening", 600);
    assert(playback.Active(600 + SessionPlaybackState::kTailUs - 1));
    observe("listening", "agent_state:listening", 700);
    assert(!playback.Active(600 + SessionPlaybackState::kTailUs));
    // A new answer during the tail cannot be opened by the old deadline.
    observe("speaking", "agent_state:speaking", 800);
    assert(playback.Active(3000000));
    playback.Reset();
    assert(!playback.Active(3000000));
    assert(!SessionPlaybackSignal("speaking", nullptr));
    assert(!SessionPlaybackSignal("unknown", "agent_state:unknown"));
}
