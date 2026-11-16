#include "eidolon/pet/pet_light.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace eidolon::pet;

namespace {
constexpr uint64_t kStep = 100;  // proximity at 10 Hz
constexpr int kLuxEvery = 5;     // lux at 2 Hz

struct Rig {
    LightRecognizer rec;
    std::vector<SenseEvent> events;
    uint64_t t = 0;
    int tick = 0;

    void Sample(uint16_t prox, float lux)
    {
        LightSample s;
        s.t_ms = t;
        s.proximity = prox;
        s.proximity_valid = true;
        if (tick % kLuxEvery == 0) {
            s.lux = lux;
            s.lux_valid = true;
        }
        ++tick;
        rec.Update(s, events);
        t += kStep;
    }
    void Feed(uint16_t prox, float lux, uint64_t ms)
    {
        for (uint64_t end = t + ms; t < end;) Sample(prox, lux);
    }
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
    void Settle() { Feed(20, 200, 5000); }
};

void TestQuietSettle()
{
    Rig r;
    r.Settle();
    assert(r.events.empty());
    assert(std::fabs(r.rec.proximity_baseline() - 20) < 1 && std::fabs(r.rec.lux_baseline() - 200) < 1);
    // Proximity jitter well inside the threshold is not a hand.
    for (int i = 0; i < 100; ++i) r.Sample(i % 2 ? 70 : 15, 200);
    assert(r.events.empty());
}

void TestHandNear()
{
    Rig r;
    r.Settle();
    r.Feed(200, 200, 500);
    assert(r.Count(SenseKind::HandNear) == 1);
    assert(std::fabs(r.First(SenseKind::HandNear)->value - 180) < 5);
    r.Feed(220, 200, 2000);
    assert(r.Count(SenseKind::HandNear) == 1);  // once per approach
    r.Feed(20, 200, 2000);
    r.Feed(250, 200, 500);
    assert(r.Count(SenseKind::HandNear) == 2);
    assert(r.Count(SenseKind::FaceClose) == 0 && r.Count(SenseKind::LightsOff) == 0);
}

void TestFaceCloseAndRetreat()
{
    Rig r;
    r.Settle();
    r.Feed(1800, 200, 200);
    assert(r.Count(SenseKind::FaceClose) == 0);  // not yet 300 ms
    r.Feed(1800, 200, 400);
    assert(r.Count(SenseKind::FaceClose) == 1);
    r.Feed(1900, 200, 2000);
    assert(r.Count(SenseKind::FaceClose) == 1);
    // Arrived straight at saturation: no HandNear, and the retreat through
    // the middle range is not an approach either.
    r.Feed(600, 200, 300);
    r.Feed(300, 200, 300);
    r.Feed(20, 200, 2000);
    assert(r.Count(SenseKind::HandNear) == 0);
    assert(r.Count(SenseKind::Covered) == 0 && r.Count(SenseKind::Uncovered) == 0);
    // Light never dropped: lights events neither.
    assert(r.Count(SenseKind::LightsOff) == 0 && r.Count(SenseKind::LightsOn) == 0);
}

void TestCoveredPeekaboo()
{
    Rig r;
    r.Settle();
    r.Feed(2047, 5, 400);
    assert(r.Count(SenseKind::Covered) == 0);
    r.Feed(2047, 5, 1200);
    assert(r.Count(SenseKind::Covered) == 1 && r.rec.covered());
    r.Feed(2047, 5, 3000);
    assert(r.Count(SenseKind::Covered) == 1);
    // Hand lifts: Uncovered, and the light coming back is not "lights on";
    // the light that went away was not "lights off".
    r.Feed(800, 150, 200);
    assert(r.Count(SenseKind::Uncovered) == 1 && !r.rec.covered());
    r.Feed(20, 200, 4000);
    assert(r.Count(SenseKind::Uncovered) == 1);
    assert(r.Count(SenseKind::LightsOff) == 0 && r.Count(SenseKind::LightsOn) == 0);
    assert(r.Count(SenseKind::HandNear) == 0);

    // Saturated but the light did not drop: FaceClose, never Covered.
    Rig f;
    f.Settle();
    f.Feed(2047, 190, 3000);
    assert(f.Count(SenseKind::Covered) == 0 && f.Count(SenseKind::FaceClose) == 1);

    // In a dark room there is no drop to see: no Covered.
    Rig d;
    d.Feed(20, 1, 5000);
    d.Feed(2047, 0, 3000);
    assert(d.Count(SenseKind::Covered) == 0);
}

void TestLightsOffOn()
{
    Rig r;
    r.Settle();
    r.Feed(20, 3, 2000);
    assert(r.Count(SenseKind::LightsOff) == 1);
    assert(std::fabs(r.First(SenseKind::LightsOff)->value - 3) < 0.01f);
    assert(r.Count(SenseKind::Covered) == 0 && r.Count(SenseKind::LightsOn) == 0);
    r.Feed(20, 3, 5000);
    assert(r.Count(SenseKind::LightsOff) == 1);
    r.Feed(20, 200, 2000);
    assert(r.Count(SenseKind::LightsOn) == 1);
    r.Feed(20, 200, 3000);
    assert(r.Count(SenseKind::LightsOn) == 1 && r.Count(SenseKind::LightsOff) == 1);

    // Brighter but still dim (2 -> 10 lux) is not lights on; 10 -> 60 is.
    Rig dim;
    dim.Feed(20, 2, 3000);
    dim.Feed(20, 10, 3000);
    assert(dim.Count(SenseKind::LightsOn) == 0);
    dim.Feed(20, 60, 2000);
    assert(dim.Count(SenseKind::LightsOn) == 1);

    // Dusk: a slow fall over 20 s is not lights off.
    Rig dusk;
    dusk.Settle();
    for (int i = 0; i <= 200; ++i) dusk.Sample(20, 200.0f - 0.9f * static_cast<float>(i));
    assert(dusk.Count(SenseKind::LightsOff) == 0);

    // A flickering lamp reports each direction once inside the cooldown.
    Rig flicker;
    flicker.Settle();
    for (int i = 0; i < 4; ++i) flicker.Feed(20, i % 2 ? 200.0f : 3.0f, 500);
    assert(flicker.Count(SenseKind::LightsOff) == 1);
}

void TestBacklightIsNotLightsOff()
{
    // The same 200 -> 50 lux step: lights off without a backlight change...
    Rig plain;
    plain.Settle();
    plain.Feed(20, 50, 3000);
    assert(plain.Count(SenseKind::LightsOff) == 1);

    // ...but nothing when the screen dimmed just before.
    Rig r;
    r.Settle();
    r.rec.IgnoreUntil(r.t + 1000);
    r.Feed(20, 50, 3000);
    assert(r.Count(SenseKind::LightsOff) == 0);
    assert(std::fabs(r.rec.lux_baseline() - 50) < 5);  // re-learned on the new scale
    r.rec.IgnoreUntil(r.t + 1000);
    r.Feed(20, 200, 3000);
    assert(r.Count(SenseKind::LightsOn) == 0);
    // Real lights off afterwards still works.
    r.Feed(20, 2, 2000);
    assert(r.Count(SenseKind::LightsOff) == 1);
}

void TestStaticObjectBecomesBaseline()
{
    Rig r;
    r.Settle();
    r.Feed(300, 200, 25000);  // a cup parked next to the sensor
    assert(r.Count(SenseKind::HandNear) == 1);
    assert(!r.rec.near());
    // Light judgements are not muted by it.
    r.Feed(300, 3, 2000);
    assert(r.Count(SenseKind::LightsOff) == 1);
    // A hand in front of the cup still reads as near.
    r.Feed(300, 3, 3000);
    r.Feed(450, 3, 500);
    assert(r.Count(SenseKind::HandNear) == 2);
}
}  // namespace

int main()
{
    TestQuietSettle();
    TestHandNear();
    TestFaceCloseAndRetreat();
    TestCoveredPeekaboo();
    TestLightsOffOn();
    TestBacklightIsNotLightsOff();
    TestStaticObjectBecomesBaseline();
    puts("PASS: light hand near, face close, covered/uncovered, lights off/on, backlight ignore, static object");
}
