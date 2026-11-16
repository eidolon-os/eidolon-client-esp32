#include "eidolon/pet/pet_motion.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace eidolon::pet;

namespace {
constexpr uint64_t kStep = 10;  // 100 Hz FIFO
constexpr float kPi = 3.14159265f;
constexpr float kDeskNoise = 0.002f;
constexpr float kHandTremor = 0.04f;

struct V {
    float x, y, z;
};
// Gravity tilted by deg from +z towards +x, unit length.
V Tilt(float deg) { return {std::sin(deg * kPi / 180), 0, std::cos(deg * kPi / 180)}; }
V Add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

struct Rig {
    explicit Rig(const MotionConfig& c = MotionConfig()) : rec(c) {}
    MotionRecognizer rec;
    std::vector<SenseEvent> events;
    uint64_t t = 0;
    uint32_t seed = 12345;

    float Noise(float amp)
    {
        seed = seed * 1664525u + 1013904223u;
        return amp * (static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f);
    }
    void Sample(V a, float noise)
    {
        ImuSample s;
        s.t_ms = t;
        s.ax = a.x + Noise(noise);
        s.ay = a.y + Noise(noise);
        s.az = a.z + Noise(noise);
        rec.Update(s, events);
        t += kStep;
    }
    void Hold(V a, float noise, uint64_t ms)
    {
        for (uint64_t end = t + ms; t < end;) Sample(a, noise);
    }
    // Rotate gravity smoothly from one tilt to another, |a| stays 1 g.
    void Rotate(float from_deg, float to_deg, float noise, uint64_t ms)
    {
        const uint64_t n = ms / kStep;
        for (uint64_t i = 0; i < n; ++i) Sample(Tilt(from_deg + (to_deg - from_deg) * i / n), noise);
    }
    void Knock(V rest, V impulse, int samples)
    {
        for (int i = 0; i < samples; ++i) Sample(Add(rest, impulse), 0);
    }
    void Shake(float amp_g, float hz, uint64_t ms)
    {
        for (uint64_t end = t + ms; t < end;) {
            const float w = std::sin(2 * kPi * hz * static_cast<float>(t) / 1000.0f);
            Sample({0.6f * amp_g * w, 0, 1 + amp_g * w}, kHandTremor);
        }
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
    void Boot() { Hold(Tilt(0), kDeskNoise, 3000); }
};

void TestQuietBoot()
{
    Rig r;
    r.Boot();
    assert(r.events.empty() && !r.rec.held() && !r.rec.tilted());
    assert(std::fabs(r.rec.magnitude() - 1.0f) < 0.01f);
}

void TestKnockIsNotPickUp()
{
    Rig r;
    r.Boot();
    const uint64_t at = r.t;
    r.Knock(Tilt(0), {0, 0, 0.8f}, 2);  // 20 ms impulse on the desk
    r.Hold(Tilt(0), kDeskNoise, 300);
    assert(r.Count(SenseKind::Knock) == 1);
    const SenseEvent* k = r.First(SenseKind::Knock);
    assert(k->t_ms == at && std::fabs(k->value - 0.8f) < 0.02f);
    assert(r.Count(SenseKind::PickedUp) == 0);

    // Drumming on the desk: each knock reported, never a pick-up, even though
    // the impulses add up to more than the pick-up deviation time.
    for (int i = 0; i < 8; ++i) {
        r.Knock(Tilt(0), {0.3f, 0, -0.6f}, 2);
        r.Hold(Tilt(0), kDeskNoise, 180);
    }
    r.Hold(Tilt(0), kDeskNoise, 300);
    assert(r.Count(SenseKind::Knock) == 9);
    assert(r.Count(SenseKind::PickedUp) == 0 && !r.rec.held());

    // A long impulse (a push or a thump that keeps moving) is not a knock.
    r.Knock(Tilt(0), {0, 0, 0.8f}, 10);
    r.Hold(Tilt(0), kDeskNoise, 500);
    assert(r.Count(SenseKind::Knock) == 9);

    // An impulse that leaves the body at another attitude is not a knock.
    r.Hold(Tilt(0), kDeskNoise, 1000);
    r.Knock(Tilt(0), {0, 0, 0.8f}, 2);
    r.Hold(Tilt(8), kDeskNoise, 400);
    assert(r.Count(SenseKind::Knock) == 9);
}

void TestKnockDurationBoundary()
{
    // Five samples (50 ms) is short enough; six (60 ms) is not "< 60 ms".
    Rig ok;
    ok.Boot();
    ok.Knock(Tilt(0), {0, 0, 0.8f}, 5);
    ok.Hold(Tilt(0), kDeskNoise, 400);
    assert(ok.Count(SenseKind::Knock) == 1);
    Rig longer;
    longer.Boot();
    longer.Knock(Tilt(0), {0, 0, 0.8f}, 6);
    longer.Hold(Tilt(0), kDeskNoise, 400);
    assert(longer.Count(SenseKind::Knock) == 0);
    // Below the peak threshold is desk vibration, not a knock.
    Rig soft;
    soft.Boot();
    soft.Knock(Tilt(0), {0, 0, 0.3f}, 2);
    soft.Hold(Tilt(0), kDeskNoise, 400);
    assert(soft.Count(SenseKind::Knock) == 0);
}

void TestKnockSuppressedDuringServoMotion()
{
    Rig r;
    r.Boot();
    r.rec.SuppressUntil(r.t + 400);
    r.Knock(Tilt(0), {0, 0, 0.8f}, 2);
    r.Hold(Tilt(0), kDeskNoise, 300);
    assert(r.Count(SenseKind::Knock) == 0);
    // After the window the rest pose is re-learned (the head moved), then
    // knocks count again.
    r.Hold(Tilt(0), kDeskNoise, 1500);
    r.Knock(Tilt(0), {0, 0, 0.8f}, 2);
    r.Hold(Tilt(0), kDeskNoise, 300);
    assert(r.Count(SenseKind::Knock) == 1);

    // A servo pitch move inside the window (turn + acceleration) is not a pick-up.
    r.rec.SuppressUntil(r.t + 1200);
    r.Knock(Tilt(0), {0, 0, 0.4f}, 15);
    r.Rotate(0, 35, kDeskNoise, 600);
    r.Hold(Tilt(35), kDeskNoise, 2000);
    assert(r.Count(SenseKind::PickedUp) == 0 && !r.rec.held());
}

void TestPickUpAndPutDown()
{
    Rig r;
    r.Boot();
    r.Hold({0, 0, 1.4f}, kHandTremor, 150);  // lift: 1.4 g for 150 ms
    r.Rotate(0, 40, kHandTremor, 300);       // and turned in the hand
    assert(r.Count(SenseKind::PickedUp) == 1 && r.rec.held());
    assert(r.First(SenseKind::PickedUp)->value > 25);
    r.Hold(Tilt(40), kHandTremor, 3000);     // held, trembling: not put down
    assert(r.Count(SenseKind::PutDown) == 0 && r.rec.held());
    // Knocks and tilts are not judged in the hand.
    r.Knock(Tilt(40), {0, 0, 0.8f}, 2);
    r.Hold(Tilt(40), kHandTremor, 300);
    assert(r.Count(SenseKind::Knock) == 0 && r.Count(SenseKind::Tilted) == 0);
    // Set down on the desk, slightly askew.
    r.Rotate(40, 5, kHandTremor, 300);
    r.Hold({0, 0, 0.8f}, 0, 30);  // touch-down bump
    r.Hold(Tilt(5), kDeskNoise, 1500);
    assert(r.Count(SenseKind::PutDown) == 1 && !r.rec.held());
    assert(r.Count(SenseKind::PickedUp) == 1);
    r.Hold(Tilt(5), kDeskNoise, 3000);
    assert(r.Count(SenseKind::Tilted) == 0);  // 5 deg is level enough
    // The new rest pose is the reference for the next pick-up.
    r.Hold(Add(Tilt(5), {0, 0, 0.4f}), kHandTremor, 150);
    r.Rotate(5, 45, kHandTremor, 300);
    assert(r.Count(SenseKind::PickedUp) == 2);
}

void TestTiltWithoutLiftIsNotPickUp()
{
    Rig r;
    r.Boot();
    r.Rotate(0, 20, kDeskNoise, 300);  // tipped by hand on the desk, |a| stays 1 g
    assert(r.Count(SenseKind::PickedUp) == 0);
    r.Hold(Tilt(20), kDeskNoise, 1500);
    assert(r.Count(SenseKind::Tilted) == 0);  // not yet 2 s at rest
    r.Hold(Tilt(20), kDeskNoise, 1500);
    assert(r.Count(SenseKind::Tilted) == 1 && r.rec.tilted());
    assert(std::fabs(r.First(SenseKind::Tilted)->value - 20) < 1.0f);
    r.Hold(Tilt(20), kDeskNoise, 5000);
    assert(r.Count(SenseKind::Tilted) == 1);  // once
    // Hysteresis: 9 deg is not level yet, 3 deg is.
    r.Rotate(20, 9, kDeskNoise, 500);
    r.Hold(Tilt(9), kDeskNoise, 3000);
    assert(r.Count(SenseKind::Level) == 0 && r.rec.tilted());
    r.Rotate(9, 3, kDeskNoise, 300);
    r.Hold(Tilt(3), kDeskNoise, 1000);
    assert(r.Count(SenseKind::Level) == 1 && !r.rec.tilted());
    assert(r.Count(SenseKind::PickedUp) == 0);

    Rig small;
    small.Boot();
    small.Rotate(0, 10, kDeskNoise, 1000);
    small.Hold(Tilt(10), kDeskNoise, 6000);
    assert(small.Count(SenseKind::Tilted) == 0);
}

void TestShaken()
{
    Rig r;
    r.Boot();
    r.Shake(1.2f, 4, 1000);
    assert(r.Count(SenseKind::Shaken) == 0);  // not sustained yet
    r.Shake(1.2f, 4, 5000);
    assert(r.Count(SenseKind::Shaken) == 1);  // one report per shaking episode
    assert(r.First(SenseKind::Shaken)->value > 0.5f);
    assert(r.Count(SenseKind::Knock) == 0);
    // Stop, then a new episode later reports again.
    r.Hold(Tilt(0), kDeskNoise, 2000);
    r.Shake(1.2f, 4, 3000);
    assert(r.Count(SenseKind::Shaken) == 2);

    // Cooldown: a second episode inside it is not reported.
    MotionConfig c;
    c.shake_cooldown_ms = 10000;
    Rig cd(c);
    cd.Boot();
    cd.Shake(1.2f, 4, 2500);
    cd.Hold(Tilt(0), kDeskNoise, 1500);
    cd.Shake(1.2f, 4, 2500);
    assert(cd.Count(SenseKind::Shaken) == 1);
    cd.Hold(Tilt(0), kDeskNoise, 5000);
    cd.Shake(1.2f, 4, 2500);
    assert(cd.Count(SenseKind::Shaken) == 2);

    // One hard jolt is not shaking; neither is gentle rocking.
    Rig j;
    j.Boot();
    j.Hold({0, 0, 2.5f}, 0, 100);
    j.Hold(Tilt(0), kDeskNoise, 3000);
    j.Shake(0.3f, 2, 4000);
    assert(j.Count(SenseKind::Shaken) == 0);
}
}  // namespace

int main()
{
    TestQuietBoot();
    TestKnockIsNotPickUp();
    TestKnockDurationBoundary();
    TestKnockSuppressedDuringServoMotion();
    TestPickUpAndPutDown();
    TestTiltWithoutLiftIsNotPickUp();
    TestShaken();
    puts("PASS: motion knock vs pick-up, servo suppression, put down, tilt/level hysteresis, shake once + cooldown");
}
