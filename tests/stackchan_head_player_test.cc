// Host tests for the StackChan local head clip player
// (main/boards/m5stack-stackchan/local_head_player.h).
#include "boards/m5stack-stackchan/local_head_player.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace eidolon::pet;
using stackchan::HeadPose;
using stackchan::LocalHeadPlayer;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

constexpr int64_t kMs = 1000;

const HeadPathLimits& Limits() {
    static const HeadPathLimits l = [] {
        HeadPathLimits x;
        x.yaw_lo = -1280; x.yaw_hi = 1280; x.pitch_lo = -40; x.pitch_hi = 270;   // StackChan: 18..49 deg, rest 22
        return x;
    }();
    return l;
}
HeadClip Clip(uint8_t priority, bool hold, std::initializer_list<HeadKey> keys) {
    HeadClip c; c.priority = priority; c.hold = hold;
    for (const auto& k : keys) c.keys[c.count++] = k;
    return c;
}
HeadKey K(uint16_t at, float yaw, float pitch, uint16_t dps = 60) {
    return HeadKey{at, static_cast<int16_t>(std::lround(yaw * 10)), static_cast<int16_t>(std::lround(pitch * 10)), dps};
}
bool Refused(const LocalHeadPlayer& p, const HeadClip& c, int64_t now, const char* why, LocalHeadPlayer::Body body = {}) {
    const char* r = p.Refusal(c, now, body);
    return r && std::strcmp(r, why) == 0;
}
// Runs the player at 20 ms ticks; returns the last pose it drove.
HeadPose Run(LocalHeadPlayer& p, int64_t& now, int64_t ms, int* home_due = nullptr) {
    HeadPose last;
    for (int64_t t = 0; t < ms; t += 20) {
        now += 20 * kMs;
        const auto s = p.Tick(now);
        if (s.drive) last = s.pose;
        if (s.home_due && home_due) ++*home_due;
    }
    return last;
}

// Every gate the body had before still refuses, in the same cases.
void TestGatesStillRefuse() {
    LocalHeadPlayer p(Limits());
    const HeadClip c = Clip(4, false, {K(600, 10, 0)});
    LocalHeadPlayer::Body b;
    CHECK(p.Refusal(c, 10 * kMs, b) == nullptr);
    b.capture_quiet = true; CHECK(Refused(p, c, 10 * kMs, "capture", b)); b = {};
    b.gesture_busy = true; CHECK(Refused(p, c, 10 * kMs, "gesture", b)); b = {};
    b.look_at_armed = true; CHECK(Refused(p, c, 10 * kMs, "look_at", b)); b = {};
    b.power_ready = false; CHECK(Refused(p, c, 10 * kMs, "power", b)); b = {};
    p.ServerMotion(100 * kMs);
    CHECK(Refused(p, c, 1900 * kMs + 99 * kMs, "server"));
    CHECK(p.Refusal(c, 2100 * kMs, {}) == nullptr);
    HeadClip empty; CHECK(Refused(p, empty, 3000 * kMs, "empty"));
}

// A reflex takes over an idle glance; an idle glance does not interrupt a reflex.
void TestPriority() {
    LocalHeadPlayer p(Limits());
    int64_t now = 5000 * kMs;
    const HeadClip reflex = Clip(2, false, {K(300, 0, 5), K(1200, 0, 0)});
    const HeadClip idle = Clip(4, false, {K(600, 12, 0)});
    p.Start(idle, now, {}, {});
    CHECK(p.Refusal(reflex, now + 100 * kMs, {}) == nullptr);
    p.Start(reflex, now + 100 * kMs, {}, {});
    CHECK(Refused(p, idle, now + 200 * kMs, "priority"));
    const HeadClip posture = Clip(1, true, {K(500, 0, 4)});
    CHECK(p.Refusal(posture, now + 200 * kMs, {}) == nullptr);   // the listening posture wins over everything local
}

// safety.stop: local clips stay off until the next server head command or the end of the
// session, however long that takes; the session-close stop does not latch.
void TestSafetyStopLatches() {
    LocalHeadPlayer p(Limits());
    const HeadClip c = Clip(2, false, {K(300, 0, 5), K(1200, 0, 0)});
    p.Start(c, 1000 * kMs, {}, {});
    p.SafetyStop(1100 * kMs);
    CHECK(!p.active() && p.stop_latched());
    CHECK(Refused(p, c, 1100 * kMs + 600000 * kMs, "stopped"));    // ten minutes later: still stopped
    p.ServerMotion(700000 * kMs);                                   // next server command
    CHECK(!p.stop_latched());
    CHECK(Refused(p, c, 700500 * kMs, "server"));                    // the usual 2 s after a server move
    CHECK(p.Refusal(c, 702100 * kMs, {}) == nullptr);
    // The session ending (a plain stop) ends the latch too.
    LocalHeadPlayer q(Limits());
    q.SafetyStop(1000 * kMs);
    q.ServerMotion(2000 * kMs);   // StackChanBody::Stop(latch = false), the session-close path
    CHECK(!q.stop_latched());
}

// A held clip stays where it ends; one that ends off-centre without hold goes home 2.5 s later.
void TestHoldAndReturnHome() {
    {
        LocalHeadPlayer p(Limits());
        int64_t now = 1000 * kMs;
        p.Start(Clip(4, true, {K(600, 12, 2)}), now, {}, {});
        int homes = 0;
        const HeadPose end = Run(p, now, 1000, &homes);
        CHECK(std::fabs(end.yaw - 120) < .01f && std::fabs(end.pitch - 20) < .01f);
        CHECK(!p.active() && !p.home_pending());
        Run(p, now, 10000, &homes);
        CHECK(homes == 0);
    }
    {
        LocalHeadPlayer p(Limits());
        int64_t now = 1000 * kMs;
        p.Start(Clip(2, false, {K(400, 0, 13)}), now, {}, {});
        int homes = 0;
        Run(p, now, 600, &homes);
        CHECK(p.home_pending() && homes == 0);
        Run(p, now, 2200, &homes);   // the path took ~406 ms: home is due 2.5 s after that
        CHECK(homes == 0);
        Run(p, now, 300, &homes);
        CHECK(homes == 1);
        p.StartHome(now, {0, 130}, {0, 125});
        const HeadPose back = Run(p, now, 1500);
        CHECK(std::fabs(back.yaw) < .01f && std::fabs(back.pitch) < .01f && !p.active());
    }
    {   // Ending at home: nothing to return from.
        LocalHeadPlayer p(Limits());
        int64_t now = 1000 * kMs;
        p.Start(Clip(2, false, {K(300, 0, 5), K(900, 0, 0)}), now, {}, {});
        Run(p, now, 1500);
        CHECK(!p.home_pending());
    }
}

// The path starts where the head was told to be (the pitch sags below that under load),
// unless it is clearly somewhere else; a take-over starts from the moving path itself.
void TestWhereThePathStarts() {
    LocalHeadPlayer p(Limits());
    const HeadClip c = Clip(4, true, {K(600, 10, 0)});
    p.Start(c, 1000 * kMs, HeadPose{0, 40}, HeadPose{5, -25});   // sagged 6.5 deg: trust the command
    CHECK(p.from().yaw == 0 && p.from().pitch == 40);
    LocalHeadPlayer q(Limits());
    q.Start(c, 1000 * kMs, HeadPose{0, 40}, HeadPose{120, -30});  // pushed 12 deg sideways
    CHECK(q.from().yaw == 120 && q.from().pitch == 40);
    LocalHeadPlayer r(Limits());
    r.Start(c, 1000 * kMs, HeadPose{0, 0}, HeadPose{0, 150});     // a server move left it up high
    CHECK(r.from().pitch == 150);
    // Take-over mid-move: from the path's own pose and speed, not from the servos.
    LocalHeadPlayer m(Limits());
    int64_t now = 1000 * kMs;
    m.Start(Clip(4, true, {K(1000, 20, 0, 40)}), now, {}, {});
    const HeadPose mid = Run(m, now, 400);
    m.Start(Clip(2, false, {K(600, 0, 5)}), now, HeadPose{-300, -300}, HeadPose{-300, -300});
    CHECK(std::fabs(m.from().yaw - mid.yaw) < 15.f);   // within one tick of travel
    CHECK(m.from().vyaw > .01f);                        // still moving right as it hands over
}

// Holding for a still stops right where the path is and cancels any return home.
void TestHoldForShutter() {
    LocalHeadPlayer p(Limits());
    int64_t now = 1000 * kMs;
    p.Start(Clip(3, false, {K(500, 0, 7), K(900, 0, 4)}), now, {}, {});
    Run(p, now, 200);
    HeadPose held;
    CHECK(p.Hold(now, held));
    CHECK(!p.active() && held.pitch > 0 && held.pitch < 70);
    int homes = 0; Run(p, now, 5000, &homes);
    CHECK(homes == 0);
    HeadPose none;
    CHECK(!p.Hold(now, none));
}

// The pitch never leaves the body's range, whatever a key asks for.
void TestStaysInRange() {
    LocalHeadPlayer p(Limits());
    int64_t now = 1000 * kMs;
    p.Start(Clip(2, false, {K(300, 0, -15), K(900, 0, 40)}), now, {}, {});
    for (int i = 0; i < 100; ++i) {
        now += 20 * kMs;
        const auto s = p.Tick(now);
        if (s.drive) CHECK(s.pose.pitch >= -40.001f && s.pose.pitch <= 270.001f);
    }
}

}  // namespace

int main() {
    TestGatesStillRefuse();
    TestPriority();
    TestSafetyStopLatches();
    TestHoldAndReturnHome();
    TestWhereThePathStarts();
    TestHoldForShutter();
    TestStaysInRange();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("stackchan_head_player: all tests passed\n");
    return 0;
}
