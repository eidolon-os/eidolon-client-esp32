// Host tests for the pet runtime (main/eidolon/pet/pet_runtime.*).
#include "eidolon/pet/pet_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

struct Sim {
    Runtime rt{42};
    RuntimeInputs in;
    uint64_t now = 1000;
    const Frame* last = nullptr;
    void Run(uint64_t ms, uint64_t step = 20) {
        for (uint64_t t = 0; t < ms; t += step) { now += step; in.now_ms = now; last = &rt.Tick(in); CHECK(Finite(*last)); }
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
// small, always ending back home, and never while the Host drives the face.
void TestSpeakingMovesTheHeadGently() {
    Sim s; s.in.phase = Phase::Responding; s.Run(300);
    HeadClip c; int clips = 0;
    for (uint64_t t = 0; t < 12000; t += 40) {
        s.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; s.Run(40, 40);
        while (s.rt.TakeHeadClip(c)) {
            ++clips;
            CHECK(c.priority == 3 && c.count == 2);
            for (size_t k = 0; k < c.count; ++k) { CHECK(std::abs(c.keys[k].yaw_dd) <= 60); CHECK(std::abs(c.keys[k].dpitch_dd) <= 50); }
            CHECK(c.keys[1].yaw_dd == 0 && c.keys[1].dpitch_dd == 0);   // ends at home
            CHECK(c.keys[1].at_ms <= 1400);
        }
    }
    CHECK(clips >= 4 && clips <= 16);   // 12 s of speech: a nod or glance every second or two
    Sim h; h.in.phase = Phase::Responding; h.in.host_active = true; h.in.host_gesture = 5; h.in.host_amount = 1; h.Run(300);
    int host_clips = 0;
    for (uint64_t t = 0; t < 8000; t += 40) { h.in.speech_dbfs = (t % 360) < 160 ? -18.f : -50.f; h.Run(40, 40); while (h.rt.TakeHeadClip(c)) ++host_clips; }
    CHECK(host_clips == 0);   // a Host plan owns the moment
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
    TestCardAndBubble();
    TestFaceStrokeLive();
    TestPowerAtBootIsNotAnEvent();
    TestIdleGlanceIsRareAndLowPriority();
    TestHeadStillWhileTheMicIsOpen();
    TestSpeakingMovesTheHeadGently();
    TestReactionsChirp();
    TestPattingKeepsItAnnoyed();
    TestNoBreathingInConversation();
    TestSpeakingFaceFollowsItsVoice();
    TestConversationGazeEases();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pet runtime tests passed\n");
    return 0;
}
