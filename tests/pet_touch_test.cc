#include "eidolon/pet/pet_touch.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace eidolon::pet;

namespace {
constexpr uint64_t kStep = 25;  // Si12T polled at 40 Hz

struct Rig {
    HeadTouchRecognizer rec;
    std::vector<SenseEvent> events;
    uint64_t t = 1000;

    void Feed(uint8_t f, uint8_t m, uint8_t b, uint64_t duration_ms)
    {
        for (uint64_t end = t + duration_ms; t < end; t += kStep) {
            HeadTouchSample s;
            s.t_ms = t;
            s.front = f;
            s.middle = m;
            s.back = b;
            rec.Update(s, events);
        }
    }
    void Release(uint64_t duration_ms) { Feed(0, 0, 0, duration_ms); }
    int Count(SenseKind k) const
    {
        int n = 0;
        for (const auto& e : events) n += e.kind == k;
        return n;
    }
    const SenseEvent* First(SenseKind k) const
    {
        for (const auto& e : events)
            if (e.kind == k) return &e;
        return nullptr;
    }
    void Clear() { events.clear(); }
};

int Taps(const Rig& r)
{
    return r.Count(SenseKind::TouchTapFront) + r.Count(SenseKind::TouchTapMiddle) +
           r.Count(SenseKind::TouchTapBack);
}

void TestPositionAndSingleTaps()
{
    Rig r;
    r.Feed(3, 0, 0, 50);
    assert(r.rec.down() && r.rec.position() == -1.0f);
    r.Feed(0, 0, 3, 0);
    r.Release(50);
    r.Clear();
    Rig p;
    p.Feed(1, 1, 0, 50);
    assert(std::fabs(p.rec.position() + 0.5f) < 1e-6f);
    p.Feed(1, 0, 1, 50);
    assert(p.rec.position() == 0.0f);
    p.Feed(0, 0, 2, 50);
    assert(p.rec.position() == 1.0f);

    // Front tap: Down, Release, TapFront; nothing else.
    Rig f;
    f.Feed(2, 0, 0, 150);
    f.Release(500);
    assert(f.Count(SenseKind::TouchDown) == 1 && f.Count(SenseKind::TouchRelease) == 1);
    assert(f.Count(SenseKind::TouchTapFront) == 1 && Taps(f) == 1);
    assert(f.Count(SenseKind::TouchHold) == 0 && f.Count(SenseKind::TouchStrokeStart) == 0);
    assert(f.events.front().kind == SenseKind::TouchDown);
    // Release precedes the tap it classifies.
    size_t rel = 0, tap = 0;
    for (size_t i = 0; i < f.events.size(); ++i) {
        if (f.events[i].kind == SenseKind::TouchRelease) rel = i;
        if (f.events[i].kind == SenseKind::TouchTapFront) tap = i;
    }
    assert(rel < tap);
    assert(!f.rec.down() && f.rec.position() == 0.0f);

    Rig m;
    m.Feed(0, 3, 0, 100);
    m.Release(500);
    assert(m.Count(SenseKind::TouchTapMiddle) == 1 && Taps(m) == 1);
    Rig b;
    b.Feed(0, 1, 3, 100);  // mostly back, a little middle bleed
    b.Release(500);
    assert(b.Count(SenseKind::TouchTapBack) == 1 && Taps(b) == 1);

    // Any still press that ends before a hold is a tap, slow ones too
    // (11-16: a 380 ms tap and 540-776 ms pats used to get no reaction).
    Rig l;
    l.Feed(0, 2, 0, 750);
    l.Release(500);
    assert(l.Count(SenseKind::TouchTapMiddle) == 1 && l.Count(SenseKind::TouchHold) == 0);
    // Past the hold time it is a hold, not also a tap.
    Rig h;
    h.Feed(0, 2, 0, 850);
    h.Release(500);
    assert(Taps(h) == 0 && h.Count(SenseKind::TouchHold) == 1);
}

void TestMoveEvents()
{
    Rig r;
    r.Feed(3, 0, 0, 100);
    r.Feed(2, 2, 0, 50);  // -1 -> -0.5: motion, but short of a stroke
    const SenseEvent* last = &r.events.back();
    assert(last->kind == SenseKind::TouchMove && last->dir == +1);
    assert(r.rec.direction() == +1);
    // Sitting still decays the direction to 0 after move_idle_ms.
    r.Feed(2, 2, 0, 500);
    assert(r.events.back().kind == SenseKind::TouchMove && r.events.back().dir == 0);
    r.Feed(3, 0, 0, 50);
    assert(r.events.back().dir == -1);
    // Every held sample after the first is a TouchMove.
    int moves = r.Count(SenseKind::TouchMove);
    assert(moves == static_cast<int>(r.events.size()) - 1 - r.Count(SenseKind::TouchHold) -
                        r.Count(SenseKind::TouchStrokeStart));
}

void TestDoubleTap()
{
    Rig r;
    r.Feed(0, 2, 0, 100);
    r.Release(150);
    r.Feed(0, 2, 0, 100);
    const uint64_t second_release = r.t;
    r.Release(300);
    // Held back while a third press could still turn it into patting.
    assert(r.Count(SenseKind::TouchDoubleTap) == 0);
    r.Release(200);
    assert(r.Count(SenseKind::TouchTapMiddle) == 1);  // the second tap is not reported alone
    assert(r.Count(SenseKind::TouchDoubleTap) == 1);
    assert(r.First(SenseKind::TouchDoubleTap)->t_ms >= second_release + 320);
    assert(r.First(SenseKind::TouchDoubleTap)->t_ms < second_release + 400);
    assert(r.Count(SenseKind::TouchRapid) == 0);

    // A third press soon after: patting, no double tap (and no info card).
    Rig t;
    for (int i = 0; i < 3; ++i) {
        t.Feed(0, 2, 0, 100);
        t.Release(150);
    }
    t.Release(600);
    assert(t.Count(SenseKind::TouchDoubleTap) == 0 && t.Count(SenseKind::TouchRapid) == 0);
    assert(t.Count(SenseKind::TouchTapMiddle) == 2);  // first and third

    // A slow deliberate double tap (11-16: 384 ms between them) still pairs.
    Rig slow;
    slow.Feed(2, 0, 0, 100);
    slow.Release(375);
    slow.Feed(2, 0, 0, 100);
    slow.Release(600);
    assert(slow.Count(SenseKind::TouchDoubleTap) == 1 && slow.Count(SenseKind::TouchTapFront) == 1);

    // Gap too long: two single taps.
    Rig s;
    s.Feed(2, 0, 0, 100);
    s.Release(500);
    s.Feed(2, 0, 0, 100);
    s.Release(500);
    assert(s.Count(SenseKind::TouchTapFront) == 2 && s.Count(SenseKind::TouchDoubleTap) == 0);

    // A hold in between breaks the pair.
    Rig h;
    h.Feed(0, 2, 0, 100);
    h.Release(150);
    h.Feed(0, 2, 0, 1000);
    h.Release(150);
    h.Feed(0, 2, 0, 100);
    h.Release(500);
    assert(h.Count(SenseKind::TouchDoubleTap) == 0 && h.Count(SenseKind::TouchTapMiddle) == 2);
}

void TestRapidOncePerBurst()
{
    Rig r;
    for (int i = 0; i < 4; ++i) {
        r.Feed(0, 3, 0, 100);
        r.Release(200);
    }
    assert(r.Count(SenseKind::TouchRapid) == 1);
    // On the way: a tap, a pair cut short by the third (nothing), a tap, then
    // the fourth is "patted too much". No double tap, so no info card.
    assert(Taps(r) == 2 && r.Count(SenseKind::TouchDoubleTap) == 0);
    // Keep tapping in the same burst: no second Rapid, and no taps either.
    for (int i = 0; i < 6; ++i) {
        r.Feed(0, 3, 0, 100);
        r.Release(200);
    }
    assert(r.Count(SenseKind::TouchRapid) == 1 && Taps(r) == 2 && r.Count(SenseKind::TouchDoubleTap) == 0);
    // A quiet gap ends the burst; the next one reports again.
    r.Release(2500);
    for (int i = 0; i < 4; ++i) {
        r.Feed(0, 3, 0, 100);
        r.Release(200);
    }
    assert(r.Count(SenseKind::TouchRapid) == 2);

    // Three taps are not rapid; four spread over more than 2 s are not either.
    Rig s;
    for (int i = 0; i < 3; ++i) {
        s.Feed(0, 3, 0, 100);
        s.Release(200);
    }
    assert(s.Count(SenseKind::TouchRapid) == 0);
    Rig slow;
    for (int i = 0; i < 4; ++i) {
        slow.Feed(0, 3, 0, 100);
        slow.Release(700);
    }
    assert(slow.Count(SenseKind::TouchRapid) == 0);
}

void TestHold()
{
    Rig r;
    r.Feed(0, 2, 0, 750);
    assert(r.Count(SenseKind::TouchHold) == 0);
    r.Feed(0, 2, 0, 100);
    assert(r.Count(SenseKind::TouchHold) == 1);
    const SenseEvent* hold = r.First(SenseKind::TouchHold);
    assert(hold->t_ms - r.First(SenseKind::TouchDown)->t_ms >= 800);
    r.Feed(0, 3, 1, 2000);  // keeps holding, slight wobble
    r.Release(200);
    assert(r.Count(SenseKind::TouchHold) == 1);  // once per press
    assert(Taps(r) == 0 && r.Count(SenseKind::TouchStrokeStart) == 0);
    // Next press can hold again.
    r.Feed(3, 0, 0, 900);
    assert(r.Count(SenseKind::TouchHold) == 2);
}

void TestStrokes()
{
    // Zones in order front -> middle -> back: with the fur.
    Rig f;
    f.Feed(2, 0, 0, 100);
    f.Feed(2, 2, 0, 100);
    f.Feed(0, 2, 2, 100);
    f.Feed(0, 0, 2, 100);
    f.Release(300);
    assert(f.Count(SenseKind::TouchStrokeStart) == 1 && f.First(SenseKind::TouchStrokeStart)->dir == +1);
    assert(f.Count(SenseKind::TouchStrokeEnd) == 1);
    const SenseEvent* end = f.First(SenseKind::TouchStrokeEnd);
    assert(end->dir == +1 && std::fabs(end->value - 0.4f) < 1e-3f);
    assert(Taps(f) == 0 && f.Count(SenseKind::TouchHold) == 0);
    assert(!f.rec.stroking());

    // Back -> front: against the fur.
    Rig b;
    b.Feed(0, 0, 3, 100);
    b.Feed(0, 3, 0, 100);
    b.Feed(3, 0, 0, 100);
    assert(b.rec.stroking());
    b.Release(100);
    assert(b.Count(SenseKind::TouchStrokeStart) == 1 && b.First(SenseKind::TouchStrokeStart)->dir == -1);
    assert(b.First(SenseKind::TouchStrokeEnd)->dir == -1);

    // A palm landing on front+middle together, then sliding back: no zone
    // order, but the travel says front -> back.
    Rig p;
    p.Feed(3, 3, 0, 100);
    p.Feed(0, 3, 0, 100);
    p.Feed(0, 1, 3, 100);
    p.Release(100);
    assert(p.Count(SenseKind::TouchStrokeStart) == 1 && p.First(SenseKind::TouchStrokeStart)->dir == +1);

    // A slow stroke that pauses is still one stroke and never a hold.
    Rig slow;
    slow.Feed(2, 0, 0, 600);
    slow.Feed(0, 2, 0, 600);
    slow.Feed(0, 0, 2, 600);
    slow.Release(100);
    assert(slow.Count(SenseKind::TouchStrokeStart) == 1 && slow.Count(SenseKind::TouchHold) == 0);
}

void TestTapIsNotStroke()
{
    // A middle tap with a little bleed into both neighbours.
    Rig r;
    r.Feed(1, 3, 0, 50);
    r.Feed(0, 3, 1, 50);
    r.Release(400);
    assert(r.Count(SenseKind::TouchStrokeStart) == 0);
    assert(r.Count(SenseKind::TouchTapMiddle) == 1);
    // Wobble within one zone while holding is not a stroke.
    Rig h;
    for (int i = 0; i < 10; ++i) {
        h.Feed(1, 3, 0, 100);
        h.Feed(0, 3, 1, 100);
    }
    h.Release(100);
    assert(h.Count(SenseKind::TouchStrokeStart) == 0);
}

// Every reported press opens with TouchDown and closes with TouchRelease;
// nothing about a press comes before its TouchDown.
void AssertWellFormed(const Rig& r)
{
    bool down = false;
    for (const auto& e : r.events) {
        switch (e.kind) {
        case SenseKind::TouchDown: assert(!down); down = true; break;
        case SenseKind::TouchMove:
        case SenseKind::TouchHold:
        case SenseKind::TouchStrokeStart: assert(down); break;
        case SenseKind::TouchRelease: assert(down); down = false; break;
        default: assert(!down); break;   // classifications follow the release
        }
    }
}

void TestFastTapsStaySeparate()
{
    // 11-16 on the real head: quick double taps and rapid pats are real
    // touches with short lifts between them, not flicker; a single released
    // sample between two presses is two taps.
    for (uint64_t gap : {25u, 50u, 100u}) {
        Rig r;
        r.Feed(3, 0, 0, 100);
        r.Release(gap);
        r.Feed(3, 0, 0, 75);
        r.Release(600);
        assert(r.Count(SenseKind::TouchTapFront) == 1 && r.Count(SenseKind::TouchDoubleTap) == 1);
        assert(r.Count(SenseKind::TouchDown) == 2 && r.Count(SenseKind::TouchRelease) == 2);
        AssertWellFormed(r);
    }
    // Five fast pats: one burst, reported once as patting too much.
    Rig pats;
    for (int i = 0; i < 5; ++i) {
        pats.Feed(0, 3, 0, 75);
        pats.Release(75);
    }
    pats.Release(600);
    assert(pats.Count(SenseKind::TouchRapid) == 1 && pats.Count(SenseKind::TouchHold) == 0);
    assert(pats.Count(SenseKind::TouchDoubleTap) == 0);
    AssertWellFormed(pats);
    // 11-16 on the head: pats of 540-776 ms among quick ones still count, so
    // the fourth pat is the annoyed one, not the ninth.
    Rig slow;
    const uint64_t pat_ms[] = {775, 100, 100, 550};
    const uint64_t gap_ms[] = {75, 75, 200, 200};
    for (int i = 0; i < 4; ++i) {
        slow.Feed(3, 1, 0, pat_ms[i]);
        slow.Release(gap_ms[i]);
    }
    assert(slow.Count(SenseKind::TouchRapid) == 1 && slow.Count(SenseKind::TouchDoubleTap) == 0);
    assert(slow.Count(SenseKind::TouchHold) == 0);
}

void TestPressSummary()
{
    // The first press has no gap before it.
    Rig r;
    r.Feed(1, 2, 0, 150);
    r.Feed(0, 3, 1, 50);
    r.Release(100);
    assert(r.rec.presses() == 1);
    const TouchPressSummary& p1 = r.rec.last_press();
    assert(p1.gap_ms == UINT32_MAX && p1.duration_ms == 200);
    assert(p1.peak[0] == 1 && p1.peak[1] == 3 && p1.peak[2] == 1);
    // The next one carries the gap since the previous release.
    r.Release(150);
    r.Feed(3, 0, 0, 100);
    r.Release(100);
    assert(r.rec.presses() == 2);
    const TouchPressSummary& p2 = r.rec.last_press();
    assert(p2.gap_ms == 250 && p2.duration_ms == 100);
    assert(p2.peak[0] == 3 && p2.peak[1] == 0 && p2.peak[2] == 0);
    // A press dropped by a servo window is not summarised, and the gap does
    // not reach back across it.
    r.rec.SuppressUntil(r.t + 100);
    r.Feed(0, 3, 0, 100);
    r.Release(100);
    r.Feed(0, 3, 0, 100);
    r.Release(100);
    assert(r.rec.presses() == 3 && r.rec.last_press().gap_ms == UINT32_MAX);
}

void TestSuppression()
{
    // Touch inside a servo window produces nothing at all.
    Rig r;
    r.rec.SuppressUntil(r.t + 500);
    r.Feed(0, 3, 0, 100);
    r.Release(100);
    r.Feed(0, 3, 0, 100);
    assert(r.events.empty());
    r.Release(400);
    assert(r.events.empty());
    // After the window a normal tap works again.
    r.Feed(0, 3, 0, 100);
    r.Release(400);
    assert(r.Count(SenseKind::TouchTapMiddle) == 1);

    // A press in progress is dropped silently when the window opens, and a
    // reading that outlives the window cannot start a press until released.
    Rig p;
    p.Feed(0, 3, 0, 200);
    assert(p.rec.down());
    p.Clear();
    p.rec.SuppressUntil(p.t + 300);
    assert(!p.rec.down());
    p.Feed(3, 3, 3, 1500);  // artifact lingers past the window
    assert(p.events.empty() && !p.rec.down());
    p.Release(100);
    p.Feed(0, 0, 2, 100);
    p.Release(100);
    assert(p.Count(SenseKind::TouchTapBack) == 1 && p.Count(SenseKind::TouchHold) == 0);

    // A tap before and a tap after a servo window do not pair into a double.
    Rig d;
    d.Feed(0, 3, 0, 100);
    d.Release(50);
    d.rec.SuppressUntil(d.t + 50);
    d.Release(100);
    d.Feed(0, 3, 0, 100);
    d.Release(400);
    assert(d.Count(SenseKind::TouchDoubleTap) == 0 && d.Count(SenseKind::TouchTapMiddle) == 2);
}

void TestStuckRecalibration()
{
    Rig r;
    r.Feed(3, 3, 3, 19900);
    assert(!r.rec.NeedsRecalibration());   // a resting palm is still a hold
    r.Feed(3, 3, 3, 300);
    assert(r.rec.NeedsRecalibration());
    // The open press is closed (no dangling TouchDown) and nothing is classified.
    assert(r.Count(SenseKind::TouchRelease) == 1 && Taps(r) == 0);
    assert(!r.rec.down());
    r.Clear();
    r.Feed(3, 3, 3, 5000);
    assert(r.events.empty());
    r.rec.ClearRecalibration();
    assert(!r.rec.NeedsRecalibration());
    // Still reading full: ignored until the pad reads released.
    r.Feed(3, 3, 3, 500);
    assert(r.events.empty());
    r.Release(100);
    r.Feed(2, 0, 0, 100);
    r.Release(400);
    assert(r.Count(SenseKind::TouchTapFront) == 1);

    // A short full-palm press is a normal touch, not a stuck chip.
    Rig palm;
    palm.Feed(3, 3, 3, 8000);
    palm.Release(100);
    assert(!palm.rec.NeedsRecalibration() && palm.Count(SenseKind::TouchHold) == 1);
}

void TestKindNames()
{
    std::set<std::string> names;
    for (int k = 0; k <= static_cast<int>(SenseKind::PowerKeyShort); ++k) {
        const char* n = SenseKindName(static_cast<SenseKind>(k));
        assert(n && *n && std::strcmp(n, "unknown") != 0);
        for (const char* c = n; *c; ++c) assert((*c >= 'a' && *c <= 'z') || *c == '_');
        names.insert(n);
    }
    assert(names.size() == static_cast<size_t>(SenseKind::PowerKeyShort) + 1);
    assert(std::strcmp(SenseKindName(SenseKind::TouchTapFront), "touch_tap_front") == 0);
    assert(std::strcmp(SenseKindName(SenseKind::PickedUp), "picked_up") == 0);
}
}  // namespace

int main()
{
    TestPositionAndSingleTaps();
    TestMoveEvents();
    TestDoubleTap();
    TestRapidOncePerBurst();
    TestHold();
    TestStrokes();
    TestTapIsNotStroke();
    TestFastTapsStaySeparate();
    TestPressSummary();
    TestSuppression();
    TestStuckRecalibration();
    TestKindNames();
    puts("PASS: head touch taps, double/rapid, fast taps, press summary, hold, strokes, suppression, stuck recalibration, kind names");
}
