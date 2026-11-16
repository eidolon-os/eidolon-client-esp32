// Host tests for the pet runtime (main/eidolon/pet/pet_runtime.*).
#include "eidolon/pet/pet_runtime.h"
#include "eidolon/pet/pet_head_path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace eidolon::pet;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

bool Finite(const Frame& f) {
    const float* p = reinterpret_cast<const float*>(&f.pose);
    static int reported = 0;
    for (size_t i = 0; i < sizeof(Pose) / sizeof(float); ++i) if (!std::isfinite(p[i])) { if (reported++ < 3) std::fprintf(stderr, "pose[%zu]=%f\n", i, p[i]); return false; }
    if (!(std::isfinite(f.blink) && std::isfinite(f.glyph_k) && std::isfinite(f.card))) { if (reported++ < 3) std::fprintf(stderr, "blink=%f gk=%f card=%f\n", f.blink, f.glyph_k, f.card); return false; }
    return true;
}

struct Played { uint64_t t; HeadClip c; HeadPathStart from; };
struct Sim {
    Runtime rt{42};
    RuntimeInputs in;
    uint64_t now = 1000;
    const Frame* last = nullptr;
    // play: act as the view and body do, taking every clip as it falls due and reporting
    // whether it played (accept); the clips are kept with the pose each started from.
    bool play = false, accept = true;
    std::vector<Played> clips;
    void Run(uint64_t ms, uint64_t step = 20) {
        for (uint64_t t = 0; t < ms; t += step) {
            now += step; in.now_ms = now; last = &rt.Tick(in); CHECK(Finite(*last));
            HeadClip c;
            while (play && rt.TakeHeadClip(c)) {
                clips.push_back({now, c, HeadPathStart{static_cast<float>(rt.head_yaw_dd()), static_cast<float>(rt.head_pitch_dd()), 0, 0}});
                rt.OnHeadClip(accept, now);
            }
        }
    }
    void Sense(SenseKind k, float value = 0, int8_t dir = 0) { rt.OnSense({k, now, value, dir}); }
};

void TestIdleLifeStaysBounded() {
    Sim s; s.Run(120000);
    const Frame& f = *s.last;
    CHECK(f.pose.gaze_x >= -1.5f && f.pose.gaze_x <= 1.5f);
    CHECK(f.blink >= 0 && f.blink <= 1);
    CHECK(s.rt.mood() == Mood::Calm);
}

void TestBlinksHappen() {
    Sim s; int closed = 0;
    for (int i = 0; i < 1000; ++i) { s.Run(20); if (s.last->blink < .5f) ++closed; }
    CHECK(closed > 0);   // 20 s of idle life contains blinks
}

void TestTapReactionAndHeadClip() {
    Sim s; s.Run(500);
    s.Sense(SenseKind::TouchTapFront);
    HeadClip clip;
    CHECK(s.rt.TakeHeadClip(clip));
    CHECK(clip.count == 3 && clip.priority == 2);
    CHECK(!s.rt.TakeHeadClip(clip));   // taken once
    s.Run(200);
    CHECK(std::strcmp(s.rt.activity(), "tap_front") == 0);
    s.Run(1500);
    CHECK(std::strcmp(s.rt.activity(), "tap_front") != 0);
}

void TestHostGestureWinsOverLocalReaction() {
    Sim s; s.Run(200);
    s.Sense(SenseKind::Shaken);
    s.Run(100);
    CHECK(std::strcmp(s.rt.activity(), "shaken") == 0);
    s.in.host_active = true; s.in.host_gesture = 7; s.in.host_amount = 1;
    s.Run(400);
    CHECK(std::strcmp(s.rt.activity(), "shaken") != 0);   // local reaction dropped, never contends
    CHECK(s.last->mouth == Mouth::O);                      // attention recipe
    CHECK(s.last->emote == Emote::Bang);
    s.in.host_active = false; s.Run(2000);
}

void TestStrokeWithAndAgainstFur() {
    Sim s; s.Run(200);
    s.Sense(SenseKind::TouchDown, -1);
    s.Sense(SenseKind::TouchStrokeStart, 0, +1);
    s.Run(800);
    CHECK(s.last->glyph == Glyph::Happy);
    s.Sense(SenseKind::TouchStrokeEnd, 1.2f, +1);
    s.Run(1500);
    CHECK(s.rt.mood() == Mood::Content);

    Sim a; a.Run(200);
    a.Sense(SenseKind::TouchDown, 1);
    a.Sense(SenseKind::TouchStrokeStart, 0, -1);
    a.Run(500);
    CHECK(a.last->glyph == Glyph::Squeeze);
    a.Sense(SenseKind::TouchStrokeEnd, .6f, -1);
    a.Run(1500);
    CHECK(a.rt.mood() == Mood::Calm);   // against the fur is not a reward
}

void TestSleepAndWake() {
    Sim s; s.in.dark = true;
    s.Run(13ull * 60 * 1000, 100);       // dark room, nobody around
    CHECK(s.rt.mood() == Mood::Asleep);
    CHECK(s.last->glyph == Glyph::Content);
    uint8_t led[36]; s.rt.Led(s.now, led);
    int lit = 0; for (uint8_t v : led) lit += v;
    CHECK(lit == 0);                      // asleep: LEDs off
    s.Sense(SenseKind::LightsOn);         // not a waking stimulus by itself? it is: wakes=true
    s.Run(100);
    CHECK(s.rt.mood() == Mood::Calm);

    Sim t; t.in.dark = true; t.Run(13ull * 60 * 1000, 100);
    t.Sense(SenseKind::Tilted, 14);       // does not wake a sleeping pet
    t.Run(100);
    CHECK(t.rt.mood() == Mood::Asleep);
    t.Sense(SenseKind::PickedUp);         // lifting does
    t.Run(100);
    CHECK(t.rt.mood() != Mood::Asleep);
}

void TestConversationKeepsCompany() {
    Sim s; s.in.dark = true; s.Run(6ull * 60 * 1000, 100);
    CHECK(s.rt.mood() == Mood::Bored);
    s.in.phase = Phase::Listening; s.in.mic_level = .6f;
    s.Run(1000);
    CHECK(s.rt.mood() == Mood::Calm);
    uint8_t led[36]; s.rt.Led(s.now, led);
    CHECK(led[5 * 3 + 1] > 0);            // VU meter lights the bottom LED green
    CHECK(s.last->tip_l > .3f && s.last->tip_l == s.last->tip_r);   // both tips follow the voice
}

void TestAntennaeCarrySessionState() {
    Sim s; s.Run(3000);
    CHECK(s.last->tip_rgb == 0);                         // idle: eye colour, no status
    s.in.phase = Phase::Thinking;
    float max_diff = 0; uint32_t think_rgb = 0;
    for (int i = 0; i < 40; ++i) { s.Run(25); max_diff = std::max(max_diff, std::fabs(s.last->tip_l - s.last->tip_r)); think_rgb = s.last->tip_rgb; }
    CHECK(max_diff > .5f);                               // thinking ping-pongs between the tips
    CHECK(s.last->emote == Emote::None);                 // no "..." beside the face any more
    s.in.phase = Phase::Listening; s.in.muted = true; s.Run(200);
    CHECK(s.last->tip_rgb != think_rgb && s.last->tip_l < .3f);   // muted: dim, own colour
    s.in.muted = false; s.in.mic_level = .8f; s.Run(500);
    const uint32_t listen_rgb = s.last->tip_rgb;
    CHECK(listen_rgb != 0 && listen_rgb != think_rgb && s.last->tip_l > .6f);
    s.rt.OnHostAction(HostAction::Failed, s.now); s.Run(60);
    CHECK(s.last->tip_rgb != listen_rgb && s.last->tip_l == 1.f);  // failed receipt flashes
    s.Run(1000);
    CHECK(s.last->tip_rgb == listen_rgb);                // and is gone after it
    s.in.phase = Phase::Ambient; s.in.host_active = true; s.in.host_gesture = 2; s.in.host_amount = 1;
    max_diff = 0;
    for (int i = 0; i < 40; ++i) { s.Run(25); max_diff = std::max(max_diff, std::fabs(s.last->tip_l - s.last->tip_r)); }
    CHECK(max_diff > .5f && s.last->emote == Emote::None);         // the Host's Ponder uses the tips too
}

void TestShutterShowsTheStillBeingTaken() {
    // The phase a still is taken in: the model is thinking, the tips ping-pong.
    Sim s; s.in.phase = Phase::Thinking; s.Run(2000);
    const float open = s.last->pose.open_l;
    const uint32_t think_rgb = s.last->tip_rgb;
    s.rt.OnCameraShutter(s.now);
    CHECK(s.rt.animating());                             // redrawn while it shows
    s.Run(Runtime::kShutterMs / 2);
    CHECK(s.last->tip_rgb == 0xFFFFFF && s.last->tip_l == 1.f && s.last->tip_r == 1.f);
    CHECK(s.last->pose.open_l < open * .7f);             // eyes narrowed, focusing
    // Muted or not, sleeping or not: the moment a picture leaves is always shown.
    s.in.muted = true; s.rt.OnCameraShutter(s.now); s.Run(60);
    CHECK(s.last->tip_rgb == 0xFFFFFF);
    s.Run(Runtime::kShutterMs + 100);
    CHECK(s.last->tip_rgb != 0xFFFFFF && !s.rt.animating());
    s.in.muted = false; s.Run(200);
    CHECK(s.last->tip_rgb == think_rgb);                 // back to what the phase shows
}

void TestCardAndBubble() {
    Sim s; s.Run(100);
    s.Sense(SenseKind::TouchDoubleTap);
    s.Run(600);
    CHECK(s.rt.card_requested(s.now));
    CHECK(s.last->card > .5f);
    s.Run(5000);
    CHECK(s.last->card < .1f);
    s.rt.OnPower(false, 50, s.now);       // first reading: just the state
    s.rt.OnPower(true, 50, s.now);
    s.Run(300);
    CHECK(s.last->bubble == Bubble::Bolt);
    uint8_t led[36]; s.rt.Led(s.now + 3000, led);
    int lit = 0; for (uint8_t v : led) lit += v;
    CHECK(lit > 0);
}

void TestPowerAtBootIsNotAnEvent() {
    Sim s; s.Run(100);
    s.rt.OnPower(true, 80, s.now);        // booted on the charger
    s.Run(4000);
    CHECK(s.rt.mood() == Mood::Calm);
    CHECK(s.last->bubble == Bubble::None);
    s.rt.OnPower(true, 81, s.now);        // still on it
    s.Run(300);
    CHECK(s.last->bubble == Bubble::None);
    s.rt.OnPower(false, 81, s.now);       // unplugged: a real event
    s.Run(300);
    CHECK(std::strcmp(s.rt.activity(), "charger_out") == 0);
}

void TestFaceStrokeLive() {
    Sim s; s.Run(100);
    s.rt.OnScreenStroke(true, 150, 5, s.now);
    s.Run(100);
    CHECK(std::strcmp(s.rt.activity(), "face_stroke") != 0);   // not yet travelled
    s.rt.OnScreenStroke(true, 190, 60, s.now);
    s.Run(600);
    CHECK(std::strcmp(s.rt.activity(), "face_stroke") == 0);
    CHECK(s.last->glyph == Glyph::Happy);
    s.rt.OnScreenStroke(false, 190, 60, s.now);
    s.Run(1200);
    CHECK(s.rt.mood() == Mood::Content);
}

void TestIdleGlanceIsRareAndLowPriority() {
    Sim s; int clips = 0; HeadClip c;
    for (int i = 0; i < 3000; ++i) { s.Run(100); if (s.rt.TakeHeadClip(c)) { ++clips; CHECK(c.priority == 4); } }
    CHECK(clips >= 2 && clips <= 14);   // 5 minutes: one every 25–60 s
}

// While the microphone is open (listening, thinking) the head holds still: the face reacts
// to a touch, the head does not, and no idle glance fires either. While it speaks the mic is
// closed and a touch may move the head again.
void TestHeadStillWhileTheMicIsOpen() {
    for (Phase ph : {Phase::Listening, Phase::Thinking}) {
        Sim s; s.Run(500); s.in.phase = ph; s.Run(200);
        HeadClip c; while (s.rt.TakeHeadClip(c)) {}
        s.Sense(SenseKind::TouchTapFront);
        CHECK(!s.rt.TakeHeadClip(c));
        s.Run(100);
        CHECK(std::strcmp(s.rt.activity(), "tap_front") == 0);
        for (int i = 0; i < 600; ++i) { s.Run(100); CHECK(!s.rt.TakeHeadClip(c)); }   // one minute
    }
    Sim r; r.Run(500); r.in.phase = Phase::Responding; r.Run(100);
    HeadClip c; while (r.rt.TakeHeadClip(c)) {}
    r.Sense(SenseKind::TouchTapFront);
    CHECK(r.rt.TakeHeadClip(c) && c.priority == 2);   // speaking: the mic is closed
    Sim a; a.Run(500); a.in.phase = Phase::Opening; a.Run(100);
    a.Sense(SenseKind::TouchTapFront);
    CHECK(a.rt.TakeHeadClip(c));   // opening a conversation, not listening yet
}

// Speaking: the head nods a little on emphasis and follows the eyes' glances aside, always
// small, always from and back to the listening posture it keeps while it talks, and never
// while the Host drives the face.
void TestSpeakingMovesTheHeadGently() {
    Sim s; s.in.phase = Phase::Responding; s.Run(300);
    HeadClip c; int clips = 0;
    for (uint64_t t = 0; t < 12000; t += 40) {
        s.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; s.Run(40, 40);
        while (s.rt.TakeHeadClip(c)) {
            ++clips;
            CHECK(c.priority == 3 && (c.count == 2 || c.count == 3) && c.hold);
            for (size_t k = 0; k < c.count; ++k) { CHECK(std::abs(c.keys[k].yaw_dd) <= 70); CHECK(c.keys[k].dpitch_dd >= 30 && c.keys[k].dpitch_dd <= 80); }
            CHECK(c.keys[c.count - 1].yaw_dd == 0 && c.keys[c.count - 1].dpitch_dd == 40);   // ends on the listening posture
            CHECK(c.keys[c.count - 1].at_ms <= 2000);
        }
    }
    CHECK(clips >= 4 && clips <= 16);   // 12 s of speech: a nod or glance every second or two
    Sim h; h.in.phase = Phase::Responding; h.in.host_active = true; h.in.host_gesture = 5; h.in.host_amount = 1; h.Run(300);
    int host_clips = 0;
    for (uint64_t t = 0; t < 8000; t += 40) { h.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; h.Run(40, 40); while (h.rt.TakeHeadClip(c)) ++host_clips; }
    CHECK(host_clips == 0);   // a Host plan owns the moment
}

// The emphasis nod really reaches its height now (it used to be cut off at ~1.7 deg).
void TestSpeakingNodReaches() {
    Sim s; s.in.phase = Phase::Responding; s.Run(300);
    HeadClip c; int nods = 0;
    for (uint64_t t = 0; t < 12000; t += 40) {
        s.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; s.Run(40, 40);
        while (s.rt.TakeHeadClip(c)) {
            if (c.count != 2) continue;
            ++nods;
            const HeadPathStart from{0, 40, 0, 0};
            float top = 0;
            for (float u = 0; u <= HeadPathEndMs(c, from); u += 2) top = std::fmax(top, SampleHeadPath(c, from, u).pitch);
            CHECK(top - 40 >= 30);                      // at least 3 deg above the posture
            CHECK(HeadPathEndMs(c, from) <= 600);       // and back within the word
        }
    }
    CHECK(nods >= 2);
}

// The eyes look first and hand over to the head: as the head is let go they have already
// moved toward where it turns, and once it arrives they are back near the middle. If the
// body refuses the clip, the eyes simply hold the look.
void TestEyesLeadHead() {
    for (bool accept : {true, false}) {
        Sim s; s.accept = accept;
        int glances = 0, led = 0, centred = 0, held = 0;
        float gaze_before = 0;
        for (int i = 0; i < 18 * 60 * 50; ++i) {
            const float g_prev = s.last ? s.last->pose.gaze_x : 0.f;
            s.Run(20);
            HeadClip c;
            if (!s.rt.TakeHeadClip(c)) { gaze_before = g_prev; continue; }
            if (c.priority != 4 || c.keys[0].max_dps != 35) { s.rt.OnHeadClip(accept, s.now); continue; }
            const int16_t from = s.rt.head_yaw_dd();
            s.rt.OnHeadClip(accept, s.now);
            ++glances;
            const float dir = c.keys[0].yaw_dd > from ? 1.f : -1.f;
            if ((s.last->pose.gaze_x - gaze_before) * dir > .02f) ++led;
            const float arrive = HeadPathKeyMs(c, HeadPathStart{static_cast<float>(from), 0, 0, 0}, 0);
            s.Run(static_cast<uint64_t>(arrive) + 200);
            if (std::fabs(s.last->pose.gaze_x) < .15f) ++centred; else ++held;
        }
        CHECK(glances >= 12);
        CHECK(led * 10 >= glances * 8);   // the eyes were already on their way
        if (accept) CHECK(centred * 10 >= glances * 8);   // and handed over to the head
        else CHECK(held * 10 >= glances * 6);             // no head: the look holds
    }
}

// D1: idle glances stay as rare as before, one every 25-60 s, never two within 25 s.
void TestIdleHeadRate() {
    Sim s; s.play = true; s.Run(18ull * 60 * 1000, 100);
    std::vector<uint64_t> at;
    for (const auto& p : s.clips) if (p.c.priority == 4 && p.c.keys[0].max_dps == 35) at.push_back(p.t);
    CHECK(at.size() >= 14 && at.size() <= 45);
    for (size_t i = 1; i < at.size(); ++i) CHECK(at[i] - at[i - 1] >= 25000);
}

// D2: a glance that stays leaves the head within +-15 deg, a passing one within +-20 deg,
// pitch inside the level band; after three quiet minutes on a heading it drifts home.
void TestHoldShiftsStayWithinBounds() {
    Sim s; s.play = true; s.Run(18ull * 60 * 1000, 100);
    int shifts = 0;
    for (const auto& p : s.clips) {
        if (p.c.priority != 4 || p.c.keys[0].max_dps != 35) continue;
        const auto& k = p.c.keys[0];
        CHECK(k.dpitch_dd >= -30 && k.dpitch_dd <= 60);
        if (p.c.count == 1) { ++shifts; CHECK(std::abs(k.yaw_dd) <= 150); }
        else { CHECK(std::abs(k.yaw_dd) <= 200); CHECK(p.c.keys[2].yaw_dd == static_cast<int16_t>(p.from.yaw)); }
        CHECK(p.c.hold);
    }
    CHECK(shifts >= 3);
    // A finger resting on the screen keeps the idle glances away but is no reason to move:
    // the held heading goes home three minutes after it was taken.
    Sim r; r.play = true;
    while (r.rt.head_yaw_dd() == 0 && r.now < 30ull * 60 * 1000) r.Run(100, 100);
    CHECK(r.rt.head_yaw_dd() != 0);
    const uint64_t since = r.now;
    r.in.pointer = true;
    const size_t before = r.clips.size();
    r.Run(4ull * 60 * 1000, 100);
    CHECK(r.clips.size() == before + 1);
    if (r.clips.size() == before + 1) {
        const auto& home = r.clips.back();
        CHECK(home.c.keys[0].yaw_dd == 0 && home.c.keys[0].dpitch_dd == 0 && home.c.keys[0].max_dps <= 20);
        CHECK(home.t >= since + 175000);
    }
    CHECK(r.rt.head_yaw_dd() == 0);
}

// Drowsy: it dozes and jerks awake at uneven intervals of 12-30 s (it used to be a fixed 7.5 s);
// the startle is the only quick move it makes then.
void TestDrowsyNodsAreIrregular() {
    Sim s; s.play = true; s.Run(500); s.rt.ForceMood(Mood::Drowsy, s.now);
    s.Run(6ull * 60 * 1000, 100);
    std::vector<uint64_t> at;
    bool posture = false;
    for (const auto& p : s.clips) {
        float peak = 0;
        for (float u = 0; u <= HeadPathEndMs(p.c, p.from); u += 2) {
            const auto x = SampleHeadPath(p.c, p.from, u);
            peak = std::fmax(peak, std::fmax(std::fabs(x.vyaw), std::fabs(x.vpitch)) * 100);
        }
        if (p.c.count == 4) { at.push_back(p.t); CHECK(peak > 25 && peak <= 60.5f); }
        else { CHECK(peak <= 25.5f); if (p.c.count == 1 && p.c.keys[0].dpitch_dd == -30 && std::abs(p.c.keys[0].yaw_dd) == 60) posture = true; }
    }
    CHECK(posture);   // first it settles low and leans over (D3)
    CHECK(at.size() >= 10);
    bool uneven = false;
    for (size_t i = 1; i < at.size(); ++i) {
        CHECK(at[i] - at[i - 1] >= 12000 && at[i] - at[i - 1] <= 30200);
        if (i > 1 && at[i] - at[i - 1] != at[i - 1] - at[i - 2]) uneven = true;
    }
    CHECK(uneven);
}

// Asleep: low and leaning over, then completely still; touched awake, the eyes open first and
// the head comes back up after.
void TestAsleepHeadStill() {
    Sim s; s.play = true; s.Run(500); s.rt.ForceMood(Mood::Asleep, s.now);
    s.Run(5000);
    CHECK(s.clips.size() == 1);
    if (!s.clips.empty()) { CHECK(s.clips[0].c.keys[0].dpitch_dd == -35 && std::abs(s.clips[0].c.keys[0].yaw_dd) == 80 && s.clips[0].c.hold); }
    s.Run(30ull * 60 * 1000, 100);
    CHECK(s.clips.size() == 1);
    const uint64_t touched = s.now;
    s.Sense(SenseKind::TouchHold);   // a hand resting on it: wakes it, no head move of its own
    s.Run(40);
    CHECK(s.rt.mood() == Mood::Calm);
    s.Run(3000);
    CHECK(s.clips.size() == 2);
    if (s.clips.size() == 2) {
        CHECK(s.clips[1].c.keys[0].yaw_dd == 0 && s.clips[1].c.keys[0].dpitch_dd == 0 && s.clips[1].c.keys[0].max_dps <= 25);
        CHECK(s.clips[1].t >= touched + 1200 + 300);   // after the reaction, eyes open first
    }
}

// Lights off: a yawn, then the head stays down (it used to pop back up 2.5 s later), and the
// drowsy posture follows.
void TestLightsOffDoesNotPopBack() {
    Sim s; s.play = true; s.Run(500);
    s.Sense(SenseKind::LightsOff);
    s.Run(200);
    CHECK(s.clips.size() == 1);
    if (!s.clips.empty()) { CHECK(s.clips[0].c.hold); CHECK(s.clips[0].c.keys[s.clips[0].c.count - 1].dpitch_dd == -35); }
    s.Run(5000);
    CHECK(s.rt.mood() == Mood::Drowsy);
    CHECK(s.clips.size() == 2 && s.clips.back().c.keys[0].dpitch_dd == -30);
}

// D4: the head is in its listening posture (+4 deg, facing you) before the microphone opens,
// both when the tap opens the conversation and after every reply; home after it ends.
void TestListeningPostureBeforeMicOpens() {
    Sim s; s.play = true; s.Run(500);
    s.rt.OnScreenTap(s.now); s.in.phase = Phase::Opening; s.Run(100);
    CHECK(s.clips.size() == 1);
    if (!s.clips.empty()) {
        const auto& c = s.clips[0].c;
        CHECK(c.hold && c.keys[c.count - 1].yaw_dd == 0 && c.keys[c.count - 1].dpitch_dd == 40);
        CHECK(HeadPathEndMs(c, HeadPathStart{}) <= 600);
    }
    s.Run(800); s.in.phase = Phase::Listening; s.Run(3000);
    CHECK(s.clips.size() == 1);   // nothing while it listens
    s.in.phase = Phase::Responding;
    for (int i = 0; i < 100; ++i) { s.in.speech_dbfs = (i % 9) < 4 ? -18.f : -50.f; s.Run(40, 40); }
    const size_t spoken = s.clips.size();
    s.in.phase = Phase::Listening; s.Run(40, 40);
    CHECK(s.clips.size() == spoken + 1);
    if (s.clips.size() == spoken + 1) {
        const auto& c = s.clips.back().c;
        CHECK(c.priority == 1 && c.hold && c.keys[0].yaw_dd == 0 && c.keys[0].dpitch_dd == 40);
        CHECK(HeadPathEndMs(c, HeadPathStart{60, 40, 0, 0}) <= 600);
    }
    s.Run(5000);
    const size_t listened = s.clips.size();
    s.in.phase = Phase::Ambient; s.Run(1400);
    CHECK(s.clips.size() == listened);   // a moment first
    s.Run(400);
    CHECK(s.clips.size() == listened + 1 && s.clips.back().c.keys[0].dpitch_dd == 0);
    // Opened without the tap (a button, a wake word): the posture comes on its own.
    Sim b; b.play = true; b.Run(500); b.in.phase = Phase::Opening; b.Run(100);
    CHECK(b.clips.size() == 1 && b.clips[0].c.priority == 1 && b.clips[0].c.keys[0].dpitch_dd == 40);
}

// D8: half as many idle glances in the dark or on battery; none on a low battery.
void TestNoIdleHeadInDarkOrLowBattery() {
    auto glances = [](Sim& s) { int n = 0; for (const auto& p : s.clips) if (p.c.priority == 4 && p.c.keys[0].max_dps == 35) ++n; return n; };
    Sim light; light.play = true; light.rt.OnPower(true, 80, light.now); light.Run(18ull * 60 * 1000, 100);
    Sim battery; battery.play = true; battery.rt.OnPower(false, 80, battery.now); battery.Run(18ull * 60 * 1000, 100);
    Sim low; low.play = true; low.rt.OnPower(false, 5, low.now); low.Run(18ull * 60 * 1000, 100);
    const int l = glances(light), b = glances(battery), w = glances(low);
    CHECK(l >= 14);
    CHECK(b * 10 <= l * 7 && b >= 4);
    CHECK(w == 0);
    // Dark: a dark room also tires it sooner (bored at 5 min); count the first five minutes.
    Sim dark; dark.play = true; dark.rt.OnPower(true, 80, dark.now); dark.in.dark = true; dark.Run(4ull * 60 * 1000 + 50000, 100);
    Sim lit; lit.play = true; lit.rt.OnPower(true, 80, lit.now); lit.Run(4ull * 60 * 1000 + 50000, 100);
    CHECK(glances(dark) < glances(lit));
}

// While a still is being taken nothing moves the head: clips waiting are dropped, and none is
// queued until the camera is done.
void TestShutterPausesHead() {
    Sim s; s.Run(500);
    s.Sense(SenseKind::TouchTapFront);
    s.rt.OnCameraShutter(s.now);
    HeadClip c;
    CHECK(!s.rt.TakeHeadClip(c));
    s.Run(300);
    s.Sense(SenseKind::Knock);
    s.Run(100);
    CHECK(!s.rt.TakeHeadClip(c));
    s.Run(Runtime::kShutterHeadQuietMs);
    s.Sense(SenseKind::Knock);
    s.Run(40);
    CHECK(s.rt.TakeHeadClip(c));
}

// Every reflex stays gentle and inside the range: nothing dips past -3.5 deg (rest is 4 deg
// above the stop), nothing peaks above 110 deg/s, and the swings really reach their size.
void TestReflexClipsAreGentleAndInRange() {
    const SenseKind kinds[] = {SenseKind::TouchTapFront, SenseKind::TouchTapMiddle, SenseKind::TouchTapBack, SenseKind::TouchRapid,
                               SenseKind::PickedUp, SenseKind::PutDown, SenseKind::Shaken, SenseKind::Knock, SenseKind::HandNear,
                               SenseKind::FaceClose, SenseKind::Uncovered, SenseKind::LightsOff, SenseKind::HeadReleased};
    int checked = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kinds) / sizeof(kinds[0])) + 3; ++i) {
        Sim s; s.Run(500);
        if (i < static_cast<int>(sizeof(kinds) / sizeof(kinds[0]))) s.Sense(kinds[i]);
        else if (i == static_cast<int>(sizeof(kinds) / sizeof(kinds[0]))) s.rt.OnPresence(true, s.now);
        else if (i == static_cast<int>(sizeof(kinds) / sizeof(kinds[0])) + 1) s.rt.OnPresence(false, s.now);
        else s.rt.OnScreenTap(s.now);
        s.Run(700);
        HeadClip c;
        if (!s.rt.TakeHeadClip(c)) { std::fprintf(stderr, "no clip for reflex %d\n", i); CHECK(false); continue; }
        ++checked;
        const HeadPathStart from;
        float peak = 0, lo = 0, hi = 0, ylo = 0, yhi = 0;
        for (float u = 0; u <= HeadPathEndMs(c, from); u += 1) {
            const auto x = SampleHeadPath(c, from, u);
            peak = std::fmax(peak, std::fmax(std::fabs(x.vyaw), std::fabs(x.vpitch)) * 100);
            lo = std::fmin(lo, x.pitch); hi = std::fmax(hi, x.pitch); ylo = std::fmin(ylo, x.yaw); yhi = std::fmax(yhi, x.yaw);
        }
        CHECK(lo >= -35.01f);
        CHECK(peak <= 110.5f);
        CHECK(HeadPathEndMs(c, from) <= 3500);
        if (i == 6) CHECK(ylo <= -69.9f && yhi >= 69.9f);   // shaken: +-7 deg, for real
    }
    CHECK(checked == 16);
}

// A speaking move still waiting when the reply ends (the view holds clips while a hang-up
// is drawn ahead of the runtime) is dropped, not played after the conversation.
void TestReplyEndDropsItsSpeakingMoves() {
    int stale = 0, pending_seen = 0;
    for (int trial = 0; trial < 300; ++trial) {
        Sim s; s.in.phase = Phase::Responding; s.Run(300);
        HeadClip c;
        const int steps = 20 + trial;
        for (int i = 0; i < steps; ++i) {
            s.in.speech_dbfs = (i % 9) < 4 ? -18.f : -50.f; s.Run(40, 40);
            if (i + 1 < steps) while (s.rt.TakeHeadClip(c)) {}
        }
        // Whatever is still waiting now (an aside's eye lead) must not outlive the reply.
        s.in.phase = Phase::Ambient; s.Run(40, 40);
        if (s.rt.TakeHeadClip(c)) { ++pending_seen; if (c.priority == 3) ++stale; }
        s.Run(3000);
        while (s.rt.TakeHeadClip(c)) if (c.priority == 3) ++stale;
    }
    CHECK(stale == 0);
    (void)pending_seen;
}

// While the view holds clips back (a start or end drawn ahead of the runtime), a reflex is
// dropped once it is a second stale; a posture still goes out when the hold ends.
void TestStaleClipsExpire() {
    Sim s; s.Run(500);
    s.Sense(SenseKind::TouchTapFront);
    s.Run(1100);
    HeadClip c;
    CHECK(!s.rt.TakeHeadClip(c));   // the tap was 1.1 s ago
    Sim p; p.Run(500);
    p.rt.ForceMood(Mood::Asleep, p.now);
    p.Run(5000);
    CHECK(p.rt.TakeHeadClip(c) && c.count == 1 && c.hold && c.keys[0].dpitch_dd == -35);
}

// A server head move ends at rest: the runtime stops assuming its own held heading.
void TestServerHeadMoveResetsHeading() {
    Sim s; s.play = true;
    while (s.rt.head_yaw_dd() == 0 && s.now < 30ull * 60 * 1000) s.Run(100, 100);
    CHECK(s.rt.head_yaw_dd() != 0);
    s.rt.OnHostAction(HostAction::Running, s.now, true);
    CHECK(s.rt.head_yaw_dd() == 0 && s.rt.head_pitch_dd() == 0);
    Sim f; f.play = true;
    while (f.rt.head_yaw_dd() == 0 && f.now < 30ull * 60 * 1000) f.Run(100, 100);
    f.rt.OnHostAction(HostAction::Done, f.now);   // a face plan's receipt: the head did not move
    CHECK(f.rt.head_yaw_dd() != 0);
}

// Each touch has its own sound; frequent ambient senses stay quiet.
void TestReactionsChirp() {
    struct Case { SenseKind kind; float value; int8_t dir; Chirp want; };
    const Case cases[] = {
        {SenseKind::TouchTapFront, 0, 0, Chirp::Bip}, {SenseKind::TouchTapMiddle, 0, 0, Chirp::Boing},
        {SenseKind::TouchTapBack, 0, 0, Chirp::Huh}, {SenseKind::TouchHold, 0, 0, Chirp::Purr},
        {SenseKind::TouchStrokeEnd, 0, +1, Chirp::Purr}, {SenseKind::TouchStrokeEnd, 0, -1, Chirp::Grumble},
        {SenseKind::TouchRapid, 0, 0, Chirp::Hmph}, {SenseKind::TouchDoubleTap, 0, 0, Chirp::Pop},
        {SenseKind::PickedUp, 0, 0, Chirp::Eep}, {SenseKind::Knock, 0, 0, Chirp::Knock},
    };
    for (const Case& k : cases) {
        Sim s; s.Run(500); Chirp c;
        while (s.rt.TakeChirp(c)) {}
        s.Sense(k.kind, k.value, k.dir);
        CHECK(s.rt.TakeChirp(c) && c == k.want);
        CHECK(!s.rt.TakeChirp(c));   // taken once
    }
    Sim q; q.Run(500); Chirp c;
    q.Sense(SenseKind::TouchDown, 0); CHECK(!q.rt.TakeChirp(c));    // the tap or stroke that follows speaks
    q.Sense(SenseKind::HandNear); CHECK(!q.rt.TakeChirp(c));        // too frequent to comment on
}

float ScaleRange(Sim& s, int frames) {
    float lo = 9, hi = -9;
    for (int i = 0; i < frames; ++i) { s.Run(50); lo = std::min(lo, s.last->pose.scale); hi = std::max(hi, s.last->pose.scale); }
    return hi - lo;
}

// Breathing redraws both eyes every step; in a conversation that is a periodic stall, so it stops.
void TestPattingKeepsItAnnoyed() {
    // Still being patted after "patted too much": every touch-down must not
    // brighten it back to the hand-on-head face.
    Sim s; s.Run(500);
    s.Sense(SenseKind::TouchRapid, 4);
    s.Run(100);
    CHECK(std::strcmp(s.rt.activity(), "rapid_taps") == 0);
    s.Sense(SenseKind::TouchDown, -1); s.Run(80);
    CHECK(std::strcmp(s.rt.activity(), "rapid_taps") == 0);
    s.Sense(SenseKind::TouchRelease, -1); s.Run(80);
    CHECK(std::strcmp(s.rt.activity(), "rapid_taps") == 0);
    // Once it has run its course, a touch is a touch again.
    s.Run(3000);
    s.Sense(SenseKind::TouchDown, -1); s.Run(40);
    CHECK(std::strcmp(s.rt.activity(), "rapid_taps") != 0);
}

void TestNoBreathingInConversation() {
    Sim s; s.in.phase = Phase::Listening; s.Run(3000);
    CHECK(ScaleRange(s, 120) < .01f);    // six seconds of listening: the eyes do not pump
    Sim a; a.Run(3000);
    CHECK(ScaleRange(a, 120) > .03f);    // at rest it still breathes
}

// Speaking: the tips follow its own voice, stressed syllables widen the eyes a little,
// and the volume setting does not change the picture once the peak has settled.
void TestSpeakingFaceFollowsItsVoice() {
    Sim s; s.in.phase = Phase::Responding; s.in.speech_dbfs = -100; s.Run(1500);
    CHECK(s.last->tip_l < .35f && s.rt.speech_level() < .02f);   // silence: dim tips
    const float rest_scale = s.last->pose.scale;
    float tip_lo = 9, tip_hi = -9, sc_hi = -9;
    for (uint64_t t = 0; t < 4000; t += 40) {
        s.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; s.Run(40, 40);
        if (t > 1000) { tip_lo = std::min(tip_lo, s.last->tip_l); tip_hi = std::max(tip_hi, s.last->tip_l); sc_hi = std::max(sc_hi, s.last->pose.scale); }
    }
    CHECK(tip_hi > .8f && tip_lo < .55f);
    CHECK(s.last->tip_l == s.last->tip_r && s.last->tip_rgb == 0);   // eye colour, both tips
    CHECK(sc_hi - rest_scale > .02f && sc_hi - rest_scale < .2f);   // a lift, not a bounce
    float ear_lo = 9, ear_hi = -9;
    for (uint64_t t = 0; t < 1440; t += 40) {
        s.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; s.Run(40, 40);
        ear_lo = std::min(ear_lo, s.last->pose.ear_l); ear_hi = std::max(ear_hi, s.last->pose.ear_l);
    }
    CHECK(ear_hi - ear_lo > .2f);   // the antennae bob with the syllables
    Sim q; q.in.phase = Phase::Responding; q.Run(500);
    float q_hi = -9;
    for (uint64_t t = 0; t < 8000; t += 40) {
        q.in.speech_dbfs = (t % 360) < 160 ? -40.f : -75.f; q.Run(40, 40);
        if (t > 5000) q_hi = std::max(q_hi, q.last->tip_l);
    }
    CHECK(q_hi > .8f);   // a quiet volume setting reaches the same brightness
}

// At the frame rate a busy audio path leaves (about 12 per second), turning from listening to
// thinking moves the eyes in small steps instead of a jump, and still arrives.
void TestConversationGazeEases() {
    Sim s; s.in.phase = Phase::Listening; s.Run(2000, 83);
    s.in.phase = Phase::Thinking;
    float prev = s.last->pose.gaze_x, max_step = 0;
    for (int i = 0; i < 16; ++i) { s.Run(83, 83); max_step = std::max(max_step, std::fabs(s.last->pose.gaze_x - prev)); prev = s.last->pose.gaze_x; }
    CHECK(max_step < .1f);
    CHECK(s.last->pose.gaze_x > .15f);
}
}  // namespace

int main() {
    TestIdleLifeStaysBounded();
    TestBlinksHappen();
    TestTapReactionAndHeadClip();
    TestHostGestureWinsOverLocalReaction();
    TestStrokeWithAndAgainstFur();
    TestSleepAndWake();
    TestConversationKeepsCompany();
    TestAntennaeCarrySessionState();
    TestShutterShowsTheStillBeingTaken();
    TestCardAndBubble();
    TestFaceStrokeLive();
    TestPowerAtBootIsNotAnEvent();
    TestIdleGlanceIsRareAndLowPriority();
    TestHeadStillWhileTheMicIsOpen();
    TestSpeakingMovesTheHeadGently();
    TestSpeakingNodReaches();
    TestEyesLeadHead();
    TestIdleHeadRate();
    TestHoldShiftsStayWithinBounds();
    TestDrowsyNodsAreIrregular();
    TestAsleepHeadStill();
    TestLightsOffDoesNotPopBack();
    TestListeningPostureBeforeMicOpens();
    TestNoIdleHeadInDarkOrLowBattery();
    TestShutterPausesHead();
    TestReflexClipsAreGentleAndInRange();
    TestServerHeadMoveResetsHeading();
    TestReplyEndDropsItsSpeakingMoves();
    TestStaleClipsExpire();
    TestReactionsChirp();
    TestPattingKeepsItAnnoyed();
    TestNoBreathingInConversation();
    TestSpeakingFaceFollowsItsVoice();
    TestConversationGazeEases();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pet runtime tests passed\n");
    return 0;
}
