// Host tests for the local head path (main/eidolon/pet/pet_head_path.h).
#include "eidolon/pet/pet_head_path.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace eidolon::pet;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

HeadClip Clip(std::initializer_list<HeadKey> keys) {
    HeadClip c;
    for (const auto& k : keys) c.keys[c.count++] = k;
    return c;
}
HeadKey K(uint16_t at, float yaw, float pitch, uint16_t dps) {
    return HeadKey{at, static_cast<int16_t>(std::lround(yaw * 10)), static_cast<int16_t>(std::lround(pitch * 10)), dps};
}
bool Near(float a, float b, float eps = .05f) { return std::fabs(a - b) <= eps; }

// The largest speed (deg/s) along the path, sampled every millisecond.
float PeakDps(const HeadClip& c, const HeadPathStart& from, const HeadPathLimits& lim = {}) {
    float peak = 0;
    const float end = HeadPathEndMs(c, from, lim);
    for (float t = 0; t <= end; t += 1) {
        const auto s = SampleHeadPath(c, from, t, lim);
        peak = std::fmax(peak, std::fmax(std::fabs(s.vyaw), std::fabs(s.vpitch)) * 100.f);   // dd/ms -> deg/s
    }
    return peak;
}

// A single move is min-jerk: it starts and stops at rest, lands exactly, and is half way at half time.
void TestSingleMoveIsMinJerk() {
    const HeadClip c = Clip({K(650, 12, 3, 35)});
    const HeadPathStart from;
    CHECK(Near(HeadPathEndMs(c, from), 650, .5f));   // 12 deg at 35 deg/s needs 643 ms; 650 authored
    auto s0 = SampleHeadPath(c, from, 0);
    CHECK(Near(s0.yaw, 0) && Near(s0.vyaw, 0, 1e-4f) && !s0.done);
    auto mid = SampleHeadPath(c, from, 325);
    CHECK(Near(mid.yaw, 60, .1f) && Near(mid.pitch, 15, .1f));
    auto end = SampleHeadPath(c, from, 650);
    CHECK(end.done && end.yaw == 120 && end.pitch == 30);
    auto near_end = SampleHeadPath(c, from, 649.5f);
    CHECK(std::fabs(near_end.vyaw) < 1e-3f);   // arrives at rest
    // Monotone: never past the target, never back.
    float prev = 0;
    for (float t = 0; t <= 650; t += 1) {
        const float y = SampleHeadPath(c, from, t).yaw;
        CHECK(y >= prev - 1e-3f && y <= 120.001f);
        prev = y;
    }
    // Peak speed: 1.875 * 12 deg / 0.65 s.
    CHECK(Near(PeakDps(c, from), 1.875f * 12 / .65f, .5f));
}

// Authored too fast for the cap: the move stretches, and every later key moves with it.
void TestSpeedCapStretchesTheMove() {
    const HeadClip c = Clip({K(200, 20, 0, 60), K(800, 20, 0, 60), K(1400, 0, 0, 60)});
    const HeadPathStart from;
    const float first = HeadPathKeyMs(c, from, 0);
    CHECK(Near(first, 187.5f * 200 / 60, .5f));    // 625 ms instead of 200
    CHECK(Near(HeadPathKeyMs(c, from, 1), first + 600, .5f));   // the hold keeps its own length
    CHECK(PeakDps(c, from) <= 60.05f);
    // No cap: the authored timing stands.
    const HeadClip fast = Clip({K(200, 20, 0, 0)});
    CHECK(Near(HeadPathEndMs(fast, from), 200, .01f));
}

// A swing through several keys: it stays inside its keys, turns back only at a key,
// and never stops in between; equal keys hold still.
void TestMultiKeyCarriesThroughAndHolds() {
    // Up through +4 to +8 (passing +4), back down: velocity at +4 is not zero.
    const HeadClip c = Clip({K(300, 0, 4, 80), K(600, 0, 8, 80), K(900, 0, 8, 80), K(1300, 0, 0, 80)});
    const HeadPathStart from;
    const float t_pass = HeadPathKeyMs(c, from, 0);
    const auto at_pass = SampleHeadPath(c, from, t_pass);
    CHECK(Near(at_pass.pitch, 40, .01f));
    CHECK(at_pass.vpitch > 0.005f);     // carried through, no stop at +4
    // Velocity continuous across the pass-through key.
    const auto before = SampleHeadPath(c, from, t_pass - .5f), after = SampleHeadPath(c, from, t_pass + .5f);
    CHECK(std::fabs(before.vpitch - after.vpitch) < .002f);
    // Holds between the two +8 keys.
    for (float t = HeadPathKeyMs(c, from, 1); t <= HeadPathKeyMs(c, from, 2); t += 5)
        CHECK(Near(SampleHeadPath(c, from, t).pitch, 80, .01f));
    // Never above +8 or below 0.
    for (float t = 0; t <= HeadPathEndMs(c, from); t += 1) {
        const float p = SampleHeadPath(c, from, t).pitch;
        CHECK(p >= -.01f && p <= 80.01f);
    }
    // A shake: turns back at every key, never past one.
    const HeadClip shake = Clip({K(220, 7, 0, 90), K(520, -7, 3, 90), K(820, 7, 0, 90), K(1120, -7, -3, 90), K(1450, 0, 0, 90)});
    float lo = 0, hi = 0;
    for (float t = 0; t <= HeadPathEndMs(shake, from); t += 1) {
        const float y = SampleHeadPath(shake, from, t).yaw;
        lo = std::fmin(lo, y); hi = std::fmax(hi, y);
    }
    CHECK(Near(lo, -70, .05f) && Near(hi, 70, .05f));   // reaches each side for real
    CHECK(PeakDps(shake, from) <= 90.05f);
}

// The path never leaves the body's range, even when a key asks for more.
void TestPitchStaysInsideTheRange() {
    HeadPathLimits lim; lim.pitch_lo = -40; lim.pitch_hi = 270;
    const HeadClip c = Clip({K(300, 0, -15, 60), K(900, 0, 5, 60), K(1600, 0, 40, 60)});
    const HeadPathStart from;
    for (float t = 0; t <= HeadPathEndMs(c, from, lim); t += 1) {
        const float p = SampleHeadPath(c, from, t, lim).pitch;
        CHECK(p >= -40.001f && p <= 270.001f);
    }
    CHECK(SampleHeadPath(c, from, 1e6f, lim).pitch == 270);
}

// The pose at a given time does not depend on how often it is asked for (a late servo tick
// does not stretch the move): sampling at 7 ms and 20 ms steps lands on the same values.
void TestSamplingRateDoesNotMatter() {
    const HeadClip c = Clip({K(400, -10, 2, 40), K(1000, -10, 2, 40), K(1700, 0, 0, 25)});
    const HeadPathStart from{30, -10, 0, 0};
    for (float t = 0; t <= 2000; t += 140) {   // 140 is a multiple of both steps
        float a = 0, b = 0;
        for (float u = 0; u <= t; u += 7) a = SampleHeadPath(c, from, u).yaw;
        for (float u = 0; u <= t; u += 20) b = SampleHeadPath(c, from, u).yaw;
        CHECK(a == b);
    }
}

// Taking over from a moving path: the new path starts with the old velocity, so the
// head does not stop dead before heading somewhere else.
void TestTakeOverKeepsVelocity() {
    const HeadClip c = Clip({K(600, 0, 0, 40)});
    const HeadPathStart moving{50, 0, .03f, 0};   // 5 deg right, moving right at 3 deg/s... per ms
    const auto s = SampleHeadPath(c, moving, 0);
    CHECK(Near(s.vyaw, .03f, 1e-4f));
    const auto e = SampleHeadPath(c, moving, HeadPathEndMs(c, moving));
    CHECK(e.done && e.yaw == 0);
}

void TestEmptyClipIsDone() {
    HeadClip c;
    const auto s = SampleHeadPath(c, HeadPathStart{12, 34, 0, 0}, 0);
    CHECK(s.done && s.yaw == 12 && s.pitch == 34);
}

}  // namespace

int main() {
    TestSingleMoveIsMinJerk();
    TestSpeedCapStretchesTheMove();
    TestMultiKeyCarriesThroughAndHolds();
    TestPitchStaysInsideTheRange();
    TestSamplingRateDoesNotMatter();
    TestTakeOverKeepsVelocity();
    TestEmptyClipIsDone();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pet_head_path: all tests passed\n");
    return 0;
}
