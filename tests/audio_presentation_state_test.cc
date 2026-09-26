#include "eidolon/audio/presentation_state.h"
#include <cassert>
using State = eidolon::AudioPresentationState;
int main() {
    State s;
    assert(s.Arm("turn", "stream", 1));
    const auto old = s.Fence();
    assert(!s.Arm("other", "other", 1));
    assert(!s.Open("unselected", true, false));
    assert(s.Open("stream", true, false));
    assert(s.Chunk("stream", 0, 640));
    assert(!s.Complete(old, true));
    assert(s.Seal("stream", true));
    assert(!s.Complete(old, true)); // trailer is not physical completion
    assert(s.BeginDrain(old));
    s.Cancel();
    assert(!s.Complete(old, true)); // late I2S return after PTT
    assert(!s.Arm("turn", "replay", 1));
    assert(s.Arm("new", "new-stream", 2));
    const auto current = s.Fence();
    assert(!s.BeginDrain(old));
    assert(!s.Chunk("stream", 1, 640));
    assert(s.Open("new-stream", true, false));
    assert(s.Chunk("new-stream", 0, 320));
    assert(s.Seal("new-stream", true));
    assert(s.BeginDrain(current));
    assert(s.Complete(current, true));
    assert(s.Bytes() == 320 && s.State() == State::Phase::Completed);
    assert(!s.Complete(current, true));
    assert(!s.Arm("past", "past-stream", 1));
    for (int fault = 0; fault < 5; ++fault) {
        State bad;
        assert(bad.Arm("t", "s", 1));
        if (fault == 0) assert(!bad.Open("s", false, false));
        else {
            assert(bad.Open("s", true, false));
            if (fault == 1) assert(!bad.Chunk("s", 1, 640));
            if (fault == 2) assert(!bad.Chunk("s", 0, 3));
            if (fault == 3) assert(!bad.Seal("s", true));
            if (fault == 4) {
                assert(bad.Chunk("s", 0, 640));
                assert(!bad.Seal("s", false));
            }
        }
        assert(bad.State() == State::Phase::Failed);
        assert(!bad.BeginDrain(bad.Fence()));
    }
}
