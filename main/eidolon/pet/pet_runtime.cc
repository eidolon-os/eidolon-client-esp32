#include "pet_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace eidolon::pet {
namespace {

constexpr float kPi = 3.14159265358979f;
float Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float Smooth(float x) { x = Clamp(x, 0, 1); return x * x * (3 - 2 * x); }
constexpr size_t kPoseFloats = sizeof(Pose) / sizeof(float);
static_assert(sizeof(Pose) % sizeof(float) == 0, "Pose must be all floats");
float* F(Pose& p) { return reinterpret_cast<float*>(&p); }
const float* F(const Pose& p) { return reinterpret_cast<const float*>(&p); }
// A conversation is open (from the tap that opens it until it ends).
bool InSession(Phase p) { return p == Phase::Opening || p == Phase::Listening || p == Phase::Thinking || p == Phase::Responding; }
// The microphone is open (half duplex: listening, and thinking until the reply starts). The head
// holds still then: servo noise reaches the mic. While the companion speaks the mic is closed, so
// the head may move (the Owner's call, 11-16).
bool MicOpen(Phase p) { return p == Phase::Listening || p == Phase::Thinking; }

// Mood timings (initial values; tuned on the device).
constexpr uint64_t kIdleToBoredMs = 20ull * 60 * 1000, kIdleToBoredDarkMs = 5ull * 60 * 1000;
constexpr uint64_t kIdleToDrowsyMs = 30ull * 60 * 1000, kIdleToDrowsyDarkMs = 8ull * 60 * 1000;
constexpr uint64_t kIdleToAsleepMs = 45ull * 60 * 1000, kIdleToAsleepDarkMs = 12ull * 60 * 1000;

struct LiveState {
    bool pad_down, pad_stroking; float pad_pos; int pad_dir; float pad_stroke_s;
    bool face_down, face_stroking; float face_x; float face_stroke_s;
};
struct ReactionOut {
    bool glyph_set = false; Glyph glyph = Glyph::None;
    bool mouth_set = false; Mouth mouth = Mouth::None;
    bool emote_set = false; Emote emote = Emote::None;
    bool led_set = false; LedMode led = LedMode::Off;
    int twitch = 0;
};
struct EmitAt { uint16_t at_ms; ParticleKind kind; uint8_t n; };
using ReactFn = void (*)(float t, Pose& p, ReactionOut& o, const LiveState& live);

struct Reaction {
    const char* name;
    uint32_t dur_ms;          // 0xFFFFFFFF = live (ends when the view says so)
    bool wakes;               // allowed to wake a sleeping pet
    bool has_after; Mood after;
    ReactFn f;
    HeadKey head[kMaxHeadKeys]; uint8_t head_count;
    EmitAt emits[3]; uint8_t emit_count;
    ParticleKind periodic_kind; uint16_t periodic_ms;
    LedMode led;
    Bubble bubble; uint16_t bubble_ms;
    bool card;
};
constexpr uint32_t kLive = 0xFFFFFFFFu;
// Head keys: (ms, yaw deg, pitch deg relative to rest, + = up).
#define K(t, y, p) HeadKey{t, static_cast<int16_t>((y) * 10), static_cast<int16_t>((p) * 10), 300}
#define KS(t, y, p, s) HeadKey{t, static_cast<int16_t>((y) * 10), static_cast<int16_t>((p) * 10), s}

enum ReactionId {
    kTapFront, kTapMiddle, kTapBack, kStrokeEndWith, kStrokeEndAgainst, kHold, kRapid, kDoubleTap,
    kPickUp, kPutDown, kShake, kKnock, kTilt, kHandNear, kFaceClose, kCovered, kUncovered,
    kLightsOff, kLightsOn, kPushed, kReleased, kChargerIn, kChargerOut, kBatteryLow,
    kOwnerHome, kOwnerAway, kTapFace, kPadLive, kFaceLive, kPowerKey, kReactionCount
};

const Reaction kReactions[kReactionCount] = {
    // kTapFront: blink-flinch, look up at the hand.
    {"tap_front", 1400, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        p.gaze_y = -.6f; p.scale = t < 160 ? -.15f : .1f; p.open_l = p.open_r = t < 140 ? .15f : 1.f;
        p.ear_l = p.ear_r = 1; p.glint = 1; },
     {K(0, 0, -5), K(300, 0, 5), K(1000, 0, 1)}, 3, {}, 0, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kTapMiddle: ears pressed down, then spring up.
    {"tap_middle", 1500, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        if (t < 300) { p.ear_l = p.ear_r = -.8f; p.squash = .25f; p.lid_bottom = .4f; }
        else { p.ear_l = p.ear_r = .9f; p.lid_bottom = .2f; p.gaze_y = -.3f; } },
     {K(0, 0, -5), K(350, 0, 4), K(1000, 0, 1)}, 3, {}, 0, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kTapBack: ears swivel back, a glance over the shoulder.
    {"tap_back", 1600, true, false, Mood::Calm, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        p.gaze_x = .45f; p.gaze_y = -.25f; p.ear_l = p.ear_r = -.4f; p.scale = .08f;
        o.emote_set = true; o.emote = Emote::Question; o.twitch = 1; },
     {K(0, 14, 3), K(1100, 0, 0)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kStrokeEndWith: afterglow of being stroked with the fur.
    {"stroke_with_fur", 900, true, true, Mood::Content, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true; o.glyph = Glyph::Happy; p.ear_l = p.ear_r = -.6f; p.blush = .35f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::PinkBreath, Bubble::None, 0, false},
    // kStrokeEndAgainst: ruffled, not amused.
    {"stroke_against_fur", 1200, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .25f; p.ear_l = .3f + .2f * std::sin(t / 70.f); p.ear_r = .3f - .2f * std::sin(t / 70.f); },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::AmberPulse, Bubble::None, 0, false},
    // kHold: a hand resting on the head: eyes close peacefully.
    {"hold", 1200, true, true, Mood::Content, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true; o.glyph = Glyph::Content; p.ear_l = p.ear_r = -.5f; p.blush = .3f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::PinkBreath, Bubble::None, 0, false},
    // kRapid: patted too much.
    {"rapid_taps", 2600, true, true, Mood::Grumpy, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .35f; p.brow_l = p.brow_r = .3f; p.brow_angle = 24; p.ear_l = p.ear_r = -1;
        p.gaze_x = -.45f; p.squash = .12f; o.emote_set = true; o.emote = Emote::Anger; },
     {K(0, -26, -5), K(2100, 0, 0)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::Amber2, Bubble::None, 0, false},
    // kDoubleTap: glance card; look down at it.
    {"glance_card", 4500, true, false, Mood::Calm, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.gaze_y = .65f; p.gaze_x = 0; p.ear_l = p.ear_r = .3f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, true},
    // kPickUp: startled, then nervous glances while held.
    {"picked_up", 2800, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        if (t < 900) { p.scale = .45f; p.squash = -.25f; p.glint = 1; p.round = .85f; p.mouth_size = .45f; p.ear_l = p.ear_r = 1;
                       o.mouth_set = true; o.mouth = Mouth::O; o.emote_set = true; o.emote = Emote::Bang; }
        else { p.scale = .15f; p.gaze_x = std::sin(t / 260.f) * .55f; p.ear_l = p.ear_r = -.3f; p.brow_l = p.brow_r = .25f; p.brow_angle = -12; } },
     {K(0, 0, 17), K(900, 0, 13)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::Amber2, Bubble::None, 0, false},
    // kPutDown: relief.
    {"put_down", 1900, true, true, Mood::Content, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        if (t < 500) { p.squash = .3f; p.ear_l = p.ear_r = -.2f; } else { p.lid_bottom = .3f; p.squash = .1f; p.ear_l = p.ear_r = -.3f; } },
     {K(0, 0, -5), K(500, 0, 1)}, 2, {{80, ParticleKind::Dust, 6}}, 1, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kShake: spiral eyes, then X eyes.
    {"shaken", 3600, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true; o.glyph = t < 2300 ? Glyph::Spiral : (t < 3100 ? Glyph::X : Glyph::None);
        p.ear_l = -.7f + .35f * std::sin(t / 90.f); p.ear_r = -.7f + .35f * std::cos(t / 90.f); },
     {K(0, 10, 0), K(330, -10, 6), K(660, 10, 0), K(990, -10, -6), K(1320, 0, 0)}, 5, {}, 0, ParticleKind::Heart, 0, LedMode::Marquee, Bubble::None, 0, false},
    // kKnock: somebody knocked on the desk.
    {"knock", 1600, true, false, Mood::Calm, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        p.scale = .2f; p.glint = 1; p.gaze_y = -.12f; p.gaze_x = 0; p.ear_l = p.ear_r = 1;
        o.emote_set = true; o.emote = Emote::Question; o.twitch = 1; },
     {K(0, 0, 8), K(900, 0, 1)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kTilt: the head has no roll axis, so the eyes counter-tilt to stay level.
    {"tilted", 3200, false, false, Mood::Calm, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.roll = .7f; p.gaze_x = .15f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kHandNear: anticipation: look up before the touch.
    {"hand_near", 1600, true, false, Mood::Calm, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.gaze_y = -.65f; p.gaze_x = .05f; p.scale = .15f; p.glint = 1; p.ear_l = p.ear_r = .9f; },
     {K(0, 0, 10), K(1200, 0, 1)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kFaceClose: cross-eyed and shy.
    {"face_close", 2600, true, true, Mood::Shy, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        p.cross = .9f; p.blush = .7f; p.ear_l = p.ear_r = -.3f; p.lid_bottom = .2f; p.mouth_size = .3f;
        o.mouth_set = true; o.mouth = Mouth::W; },
     {K(0, 0, -5), K(2100, 0, 0)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::PinkBreath, Bubble::None, 0, false},
    // kCovered: "where did you go?"
    {"covered", 2600, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        p.gaze_x = std::sin(t / 300.f) * .6f; p.gaze_y = -.1f; p.brow_l = p.brow_r = .3f; p.brow_angle = -14;
        p.ear_l = p.ear_r = .6f; o.emote_set = true; o.emote = Emote::Question; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kUncovered: peekaboo!
    {"uncovered", 1800, true, true, Mood::Happy, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true; o.glyph = Glyph::Star; p.ear_l = p.ear_r = 1; p.scale = .15f; p.mouth_size = .5f;
        o.mouth_set = true; o.mouth = Mouth::V; },
     {K(0, 0, 10), K(800, 0, 1)}, 2, {{0, ParticleKind::Spark, 10}}, 1, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kLightsOff: a yawn, then off to sleep.
    {"lights_off", 3200, false, true, Mood::Drowsy, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        p.lid_top_l = p.lid_top_r = std::min(.85f, t / 3200.f); p.ear_l = p.ear_r = -.7f;
        p.mouth_size = t < 1600 ? .8f : 0.f; if (t < 1600) { o.mouth_set = true; o.mouth = Mouth::O; } },
     {K(0, 0, 7), KS(1400, 0, -15, 120)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::Moon, 3000, false},
    // kLightsOn: squint at the light, wake.
    {"lights_on", 1900, true, true, Mood::Calm, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .4f; p.lid_bottom = .22f; p.ear_l = p.ear_r = .4f;
        const bool b = std::fmod(t, 500.f) < 90.f; p.open_l = p.open_r = b ? .1f : 1.f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kPushed: someone turns the head by hand: resist a little, then go along.
    {"head_pushed", 2400, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true;
        if (t < 1100) { o.glyph = Glyph::Squeeze; p.brow_l = p.brow_r = .2f; p.brow_angle = 16; p.ear_l = p.ear_r = -.6f; }
        else { o.glyph = Glyph::Happy; p.blush = .4f; p.ear_l = -.2f; p.ear_r = .4f; } },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Amber2, Bubble::None, 0, false},
    // kReleased: back home with a little shake of the ears.
    {"head_released", 1200, true, false, Mood::Calm, [](float, Pose& p, ReactionOut& o, const LiveState&) {
        p.ear_l = p.ear_r = .3f; o.twitch = -1; },
     {K(0, 0, 0)}, 1, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kChargerIn: "dinner".
    {"charger_in", 2600, false, true, Mood::Content, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        p.lid_bottom = .35f; p.squash = .1f; p.ear_l = p.ear_r = .2f; p.mouth_size = .3f + .15f * std::sin(t / 120.f);
        o.mouth_set = true; o.mouth = Mouth::W; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Charge, Bubble::Bolt, 4500, false},
    // kChargerOut: being carried away?
    {"charger_out", 1700, false, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        p.ear_l = p.ear_r = 1; p.gaze_x = std::sin(t / 280.f) * .5f; p.scale = .1f; o.emote_set = true; o.emote = Emote::Question; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kBatteryLow: hungry and sleepy.
    {"battery_low", 1800, false, true, Mood::Drowsy, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .5f; p.ear_l = p.ear_r = -.6f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::AmberDim, Bubble::Battery, 5000, false},
    // kOwnerHome: welcome home.
    {"owner_home", 3200, true, true, Mood::Happy, [](float t, Pose& p, ReactionOut& o, const LiveState&) {
        o.glyph_set = true; o.glyph = Glyph::Happy; p.blush = .4f; p.ear_l = std::sin(t / 100.f); p.ear_r = -std::sin(t / 100.f); },
     {K(0, 0, 17), K(200, -15, 15), K(340, 15, 15), K(480, -9, 13), K(610, 6, 13), K(740, 0, 0)}, 6,
     {{200, ParticleKind::Heart, 3}, {900, ParticleKind::Heart, 2}}, 2, ParticleKind::Heart, 0, LedMode::Welcome, Bubble::None, 0, false},
    // kOwnerAway.
    {"owner_away", 2600, false, true, Mood::Bored, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .35f; p.lid_angle_l = p.lid_angle_r = -12; p.ear_l = p.ear_r = -.65f; p.gaze_y = .3f; },
     {K(0, 0, -15), K(1200, 0, 0)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
    // kTapFace: a conversation is opening: perk up.
    {"tap_face", 900, true, false, Mood::Calm, [](float, Pose& p, ReactionOut&, const LiveState&) {
        p.scale = .32f; p.glint = 1; p.ear_l = p.ear_r = 1; },
     {K(0, 0, 13), K(700, 0, 2)}, 2, {}, 0, ParticleKind::Heart, 0, LedMode::SkinFlash, Bubble::None, 0, false},
    // kPadLive: hand on the head pad, live.
    {"head_touch", kLive, true, false, Mood::Calm, [](float t, Pose& p, ReactionOut& o, const LiveState& L) {
        if (L.pad_down && L.pad_stroking) {
            const float ts = L.pad_stroke_s * 1000.f;
            if (L.pad_dir >= 0) {
                o.glyph_set = true; o.glyph = ts > 300 ? Glyph::Happy : Glyph::None;
                p.blush = .55f * Clamp(ts / 600.f, 0, 1); p.ear_l = p.ear_r = -.9f; p.gaze_x = 0; p.gaze_y = -.25f;
                o.led_set = true; o.led = LedMode::PinkBreath;
            } else {
                o.glyph_set = true; o.glyph = Glyph::Squeeze; p.brow_l = p.brow_r = .2f; p.brow_angle = 14;
                p.ear_l = .8f + .2f * std::sin(ts / 70.f); p.ear_r = .8f - .2f * std::sin(ts / 70.f);
                o.led_set = true; o.led = LedMode::AmberPulse;
            }
            return;
        }
        if (L.pad_down && t > 800) { o.glyph_set = true; o.glyph = Glyph::Content; p.ear_l = p.ear_r = -.5f; p.blush = .3f; return; }
        const int z = L.pad_pos < -1.f / 3 ? 0 : (L.pad_pos > 1.f / 3 ? 2 : 1);
        static const float kEar[3] = {1.f, -.3f, -.5f};
        p.gaze_y = -.6f; p.scale = .1f; p.glint = 1; p.ear_l = p.ear_r = kEar[z]; },
     {}, 0, {}, 0, ParticleKind::Heart, 900, LedMode::SkinFlash, Bubble::None, 0, false},
    // kFaceLive: finger stroking the face, live.
    {"face_stroke", kLive, true, false, Mood::Calm, [](float, Pose& p, ReactionOut& o, const LiveState& L) {
        const float ts = L.face_stroke_s * 1000.f;
        o.glyph_set = true; o.glyph = ts > 300 ? Glyph::Happy : Glyph::None;
        p.blush = .65f * Clamp(ts / 600.f, 0, 1); p.ear_l = p.ear_r = -.85f;
        p.gaze_x = Clamp((L.face_x - 160.f) / 200.f, -1, 1); p.gaze_y = -.08f; },
     {}, 0, {}, 0, ParticleKind::Heart, 900, LedMode::PinkBreath, Bubble::None, 0, false},
    // kPowerKey: quiet mode.
    {"quiet_mode", 1600, false, true, Mood::Drowsy, [](float t, Pose& p, ReactionOut&, const LiveState&) {
        p.lid_top_l = p.lid_top_r = .5f * Smooth(t / 600.f); p.ear_l = p.ear_r = -.5f; },
     {}, 0, {}, 0, ParticleKind::Heart, 0, LedMode::Off, Bubble::None, 0, false},
};

// What each reaction sounds like (pet_sound.h); None stays silent. Frequent or ambient
// reactions (a hand approaching, a tilt, the face being stroked) are silent on purpose:
// a pet that comments on everything gets switched off.
const Chirp kReactionChirp[kReactionCount] = {
    Chirp::Bip,       // kTapFront
    Chirp::Boing,     // kTapMiddle
    Chirp::Huh,       // kTapBack
    Chirp::Purr,      // kStrokeEndWith
    Chirp::Grumble,   // kStrokeEndAgainst
    Chirp::Purr,      // kHold
    Chirp::Hmph,      // kRapid
    Chirp::Pop,       // kDoubleTap
    Chirp::Eep,       // kPickUp
    Chirp::Phew,      // kPutDown
    Chirp::Dizzy,     // kShake
    Chirp::Knock,     // kKnock
    Chirp::None,      // kTilt
    Chirp::None,      // kHandNear
    Chirp::Giggle,    // kFaceClose
    Chirp::Hmm,       // kCovered
    Chirp::Peekaboo,  // kUncovered
    Chirp::Yawn,      // kLightsOff
    Chirp::Wake,      // kLightsOn
    Chirp::Grumble,   // kPushed
    Chirp::None,      // kReleased
    Chirp::Yum,       // kChargerIn
    Chirp::None,      // kChargerOut
    Chirp::Sad,       // kBatteryLow
    Chirp::None,      // kOwnerHome (the wake cue already plays)
    Chirp::None,      // kOwnerAway
    Chirp::None,      // kTapFace (the session's own cue plays)
    Chirp::None,      // kPadLive (the tap or stroke that follows speaks)
    Chirp::None,      // kFaceLive
    Chirp::None,      // kPowerKey
};
#undef K
#undef KS

// face-core-1 recipes in pet form: Attend, Affirm, Ponder, Question, Soften, Delight, Hesitate, Attention.
void HostRecipe(uint8_t g, Pose& p, ReactionOut& o) {
    switch (g) {
    case 0: p.scale = .35f; p.ear_l = p.ear_r = 1; break;
    case 1: p.lid_bottom = .55f; p.squash = .12f; p.ear_l = p.ear_r = -.2f; break;
    case 2: p.gaze_x = .7f; p.gaze_y = -.25f; p.open_l = p.open_r = .75f; p.ear_l = .2f; p.ear_r = .85f; break;   // antennae show it
    case 3: p.open_l = .62f; p.lid_top_l = .2f; p.scale = .15f; p.brow_l = .4f; p.brow_angle = -8; p.ear_l = .4f; p.ear_r = 1;
            o.emote_set = true; o.emote = Emote::Question; break;
    case 4: p.lid_top_l = p.lid_top_r = .35f; p.lid_angle_l = p.lid_angle_r = -12; p.lid_bottom = .15f;
            p.ear_l = p.ear_r = -.45f; p.brow_l = p.brow_r = .2f; p.brow_angle = -12; break;
    case 5: p.blush = .5f; p.ear_l = p.ear_r = .9f; o.glyph_set = true; o.glyph = Glyph::Happy; break;
    case 6: p.gaze_x = -.5f; p.gaze_y = .15f; p.open_l = p.open_r = .72f; p.scale = -.1f; p.mouth_size = .3f;
            p.ear_l = -.3f; p.ear_r = .2f; p.brow_l = p.brow_r = .2f; p.brow_angle = -10;
            o.mouth_set = true; o.mouth = Mouth::Wave; break;
    case 7: p.scale = .6f; p.squash = -.15f; p.mouth_size = .3f; p.ear_l = p.ear_r = 1;
            o.mouth_set = true; o.mouth = Mouth::O; o.emote_set = true; o.emote = Emote::Bang; break;
    default: break;
    }
}
const uint32_t kHostLed[8] = {0xC5ECCD, 0xC5ECCD, 0x6FA8FF, 0x6FA8FF, 0xFF9DB5, 0xFFC98A, 0xA9B4C2, 0xFFD9A0};

void MoodRecipe(Mood m, Pose& p, Glyph& glyph, Emote& emote) {
    switch (m) {
    case Mood::Calm: break;
    case Mood::Content: p.lid_bottom = .28f; p.squash = .1f; p.glint = .5f; p.ear_l = p.ear_r = -.2f; break;
    case Mood::Happy: p.blush = .45f; p.ear_l = p.ear_r = -.3f; glyph = Glyph::Happy; break;
    case Mood::Curious: p.scale = .12f; p.open_r = .8f; p.lid_top_r = .12f; p.gaze_x = .4f; p.gaze_y = -.15f; p.glint = 1;
                        p.ear_l = .5f; p.ear_r = 1; break;
    case Mood::Bored: p.lid_top_l = p.lid_top_r = .38f; p.gaze_y = .35f; p.glint = .3f; p.squash = .08f; p.ear_l = p.ear_r = -.45f; break;
    case Mood::Drowsy: p.lid_top_l = p.lid_top_r = .62f; p.gaze_y = .25f; p.glint = .2f; p.ear_l = p.ear_r = -.65f; break;
    case Mood::Asleep: p.gaze_y = .2f; p.glint = 0; p.ear_l = p.ear_r = -.8f; glyph = Glyph::Content; break;
    case Mood::Grumpy: p.lid_top_l = p.lid_top_r = .3f; p.brow_l = p.brow_r = .25f; p.brow_angle = 22; p.squash = .12f;
                       p.gaze_x = -.35f; p.ear_l = p.ear_r = -1; emote = Emote::Anger; break;
    case Mood::Shy: p.gaze_x = -.45f; p.gaze_y = .35f; p.blush = .85f; p.lid_bottom = .3f; p.scale = -.1f; p.ear_l = p.ear_r = -.5f; break;
    }
}

// Idle micro-behaviours (§4.13): eyes and ears only.
enum ActId { kLook, kDblBlink, kFlick, kTiltHead, kSquint, kDream, kHop, kWiggle, kHum, kScan, kYawn, kSneeze, kActCount };
const char* const kActNames[kActCount] = {"look", "double_blink", "ear_flick", "head_tilt", "squint", "daydream", "hop", "ear_wiggle", "hum", "scan", "yawn", "sneeze"};
const uint16_t kActDur[kActCount][2] = {{1200, 2000}, {450, 450}, {300, 300}, {1200, 1500}, {800, 1100}, {1000, 1500},
                                        {650, 650}, {900, 1100}, {2000, 2400}, {2400, 2800}, {2000, 2400}, {1200, 1200}};
const bool kActRaw[kActCount] = {false, false, false, false, false, false, true, false, false, false, false, true};
// Weights per mood (Calm, Content, Happy, Curious, Bored, Drowsy); others have no idle acts.
const float kActWeights[6][kActCount] = {
    {5, 2, 2, 2, 1, 1, 1, 1, 0, 2, 0, .15f},
    {3, 2, 2, 1, 2, 2, 0, 2, 2, 0, .6f, .15f},
    {2, 2, 2, 2, 3, 0, 3, 3, 2, 0, 0, .15f},
    {3, 0, 2, 3, 0, 0, 0, 0, 0, 2, 0, 0},
    {2, 0, 1, 0, 0, 3, 0, 0, 0, 2, 3, 0},
    {0, 0, .5f, 0, 0, 1, 0, 0, 0, 0, 3, 0},
};
const uint16_t kActGap[6][2] = {{2500, 5500}, {3000, 6000}, {1800, 4000}, {2000, 4000}, {4000, 8000}, {5000, 9000}};
int MoodActRow(Mood m) {
    switch (m) {
    case Mood::Calm: return 0; case Mood::Content: return 1; case Mood::Happy: return 2;
    case Mood::Curious: return 3; case Mood::Bored: return 4; case Mood::Drowsy: return 5; default: return -1;
    }
}

void Mix(Pose& p, const Pose& target, float amt) {
    float* a = F(p); const float* b = F(target);
    for (size_t i = 0; i < kPoseFloats; ++i) a[i] += (b[i] - a[i]) * amt;
}
uint32_t MixRgb(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { const float x = ((a >> s) & 255), y = ((b >> s) & 255); return static_cast<uint32_t>(x + (y - x) * t + .5f) & 255; };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

}  // namespace

const char* MoodName(Mood mood) {
    static const char* const kNames[] = {"calm", "content", "happy", "curious", "bored", "drowsy", "asleep", "grumpy", "shy"};
    return kNames[static_cast<int>(mood)];
}

Runtime::Runtime(uint32_t seed) : rng_(seed ? seed : 1u) {}

uint32_t Runtime::Next() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
float Runtime::Rand(float a, float b) { return a + (b - a) * (Next() % 10000) / 10000.f; }

void Runtime::SetMood(Mood mood, uint64_t now_ms) {
    if (mood == mood_) return;
    mood_ = mood; mood_since_ = now_ms;
    vel_.scale += 2.2f;   // a new expression pops in
    act_.index = -1;
}

void Runtime::ForceMood(Mood mood, uint64_t now_ms) { SetMood(mood, now_ms); Interact(now_ms); }

void Runtime::StartReaction(int index, uint64_t now_ms) {
    if (index < 0 || index >= kReactionCount) return;
    const Reaction& r = kReactions[index];
    if ((mood_ == Mood::Asleep || mood_ == Mood::Drowsy) && r.wakes) SetMood(Mood::Calm, now_ms);
    else if (mood_ == Mood::Asleep && !r.wakes) return;   // only touch, lifting and strong stimuli wake it
    reaction_ = ReactionState{};
    reaction_.index = index; reaction_.start = now_ms; reaction_.last_periodic = now_ms;
    reaction_.end_at = r.dur_ms == kLive ? 0 : now_ms + r.dur_ms;
    activity_ = r.name;
    act_.index = -1;
    vel_.scale += 1.2f;
    Interact(now_ms);
    if (r.head_count && !MicOpen(phase_)) {
        pending_clip_ = HeadClip{};
        pending_clip_.count = r.head_count;
        pending_clip_.priority = 2;
        for (size_t i = 0; i < r.head_count; ++i) pending_clip_.keys[i] = r.head[i];
        clip_pending_ = true;
    }
    if (r.bubble != Bubble::None) { bubble_ = r.bubble; bubble_t0_ = now_ms; bubble_until_ = now_ms + r.bubble_ms; }
    if (r.card) card_until_ = now_ms + r.dur_ms;
    if (kReactionChirp[index] != Chirp::None) pending_chirp_ = kReactionChirp[index];
}

bool Runtime::TakeChirp(Chirp& out) {
    if (pending_chirp_ == Chirp::None) return false;
    out = pending_chirp_; pending_chirp_ = Chirp::None; return true;
}

void Runtime::QueueSpeakingClip(const HeadKey& a, const HeadKey& b) {
    if (clip_pending_) return;   // a reaction's clip waiting to play wins
    pending_clip_ = HeadClip{};
    pending_clip_.count = 2; pending_clip_.priority = 3;
    pending_clip_.keys[0] = a; pending_clip_.keys[1] = b;
    clip_pending_ = true;
}

void Runtime::OnSense(const SenseEvent& e) {
    const uint64_t now = e.t_ms;
    switch (e.kind) {
    case SenseKind::TouchDown:
        pad_down_ = true; pad_stroking_ = false; pad_pos_ = e.value; pad_dir_ = 0; pad_t0_ = now;
        twitch_side_ = e.value < -.33f ? -1 : (e.value > .33f ? 1 : 0); twitch_t0_ = now;
        // Patted too much and still being patted: stay annoyed rather than
        // brighten at every touch.
        if (reaction_.index != kRapid) StartReaction(kPadLive, now);
        break;
    case SenseKind::TouchMove: pad_pos_ = e.value; if (e.dir) pad_dir_ = e.dir; break;
    case SenseKind::TouchStrokeStart: pad_stroking_ = true; pad_dir_ = e.dir; pad_stroke_t0_ = now; break;
    case SenseKind::TouchStrokeEnd:
        pad_down_ = false; pad_stroking_ = false;
        StartReaction(e.dir >= 0 ? kStrokeEndWith : kStrokeEndAgainst, now);
        break;
    case SenseKind::TouchHold: StartReaction(kHold, now); break;
    case SenseKind::TouchRelease:
        pad_down_ = false;
        if (reaction_.index == kPadLive) { reaction_.index = -1; activity_ = ""; }
        break;
    case SenseKind::TouchTapFront: pad_down_ = false; StartReaction(kTapFront, now); break;
    case SenseKind::TouchTapMiddle: pad_down_ = false; StartReaction(kTapMiddle, now); break;
    case SenseKind::TouchTapBack: pad_down_ = false; StartReaction(kTapBack, now); break;
    case SenseKind::TouchDoubleTap: pad_down_ = false; StartReaction(kDoubleTap, now); break;
    case SenseKind::TouchRapid: pad_down_ = false; StartReaction(kRapid, now); break;
    case SenseKind::PickedUp: StartReaction(kPickUp, now); break;
    case SenseKind::PutDown: StartReaction(kPutDown, now); break;
    case SenseKind::Shaken: StartReaction(kShake, now); break;
    case SenseKind::Knock: StartReaction(kKnock, now); break;
    case SenseKind::Tilted: StartReaction(kTilt, now); break;
    case SenseKind::Level: break;
    case SenseKind::HandNear: StartReaction(kHandNear, now); break;
    case SenseKind::FaceClose: StartReaction(kFaceClose, now); break;
    case SenseKind::Covered: StartReaction(kCovered, now); break;
    case SenseKind::Uncovered: StartReaction(kUncovered, now); break;
    case SenseKind::LightsOff: StartReaction(kLightsOff, now); break;
    case SenseKind::LightsOn: StartReaction(kLightsOn, now); break;
    case SenseKind::HeadPushed: StartReaction(kPushed, now); break;
    case SenseKind::HeadReleased: StartReaction(kReleased, now); break;
    case SenseKind::HeadBlocked: break;
    case SenseKind::ChargerIn: charging_ = true; StartReaction(kChargerIn, now); break;
    case SenseKind::ChargerOut: charging_ = false; StartReaction(kChargerOut, now); break;
    case SenseKind::BatteryLow: battery_low_ = true; StartReaction(kBatteryLow, now); break;
    case SenseKind::PowerKeyShort: StartReaction(kPowerKey, now); break;
    default: break;
    }
}

void Runtime::OnPresence(bool awake, uint64_t now_ms) { StartReaction(awake ? kOwnerHome : kOwnerAway, now_ms); }
void Runtime::OnScreenTap(uint64_t now_ms) { StartReaction(kTapFace, now_ms); }

void Runtime::OnScreenStroke(bool down, float x, float travelled_px, uint64_t now_ms) {
    face_x_ = x;
    if (down) {
        if (!face_down_) { face_down_ = true; face_stroking_ = false; }
        if (!face_stroking_ && travelled_px > 30) { face_stroking_ = true; face_stroke_t0_ = now_ms; StartReaction(kFaceLive, now_ms); }
        return;
    }
    face_down_ = false;
    if (face_stroking_) {
        face_stroking_ = false;
        if (reaction_.index == kFaceLive) reaction_.end_at = now_ms + 700;   // afterglow
    }
}

void Runtime::OnPower(bool charging, int battery_pct, uint64_t now_ms) {
    if (!power_known_) {
        // The first reading is the state the pet woke up in, not an event: being
        // on the charger at boot is not "dinner".
        power_known_ = true;
        charging_ = charging;
        battery_low_ = !charging && battery_pct >= 0 && battery_pct <= 10;
        return;
    }
    if (charging != charging_) OnSense({charging ? SenseKind::ChargerIn : SenseKind::ChargerOut, now_ms, 0, 0});
    const bool low = !charging && battery_pct >= 0 && battery_pct <= 10;
    if (low && !battery_low_) OnSense({SenseKind::BatteryLow, now_ms, static_cast<float>(battery_pct), 0});
    if (!low) battery_low_ = false;
}

bool Runtime::TakeHeadClip(HeadClip& out) {
    if (!clip_pending_) return false;
    out = pending_clip_; clip_pending_ = false; return true;
}

bool Runtime::animating() const {
    return reaction_.index >= 0 || act_.index >= 0 || glyph_cur_ != glyph_target_;
}

namespace {
constexpr uint32_t kTipListen = 0x7CE0A8;   // same mint as the LED VU meter
constexpr uint32_t kTipThink = 0x9CC8FF;
constexpr uint32_t kTipMuted = 0xE07A7A;
constexpr uint32_t kTipError = 0xFF6B6B;
}  // namespace

void Runtime::OnHostAction(HostAction action, uint64_t now_ms) {
    action_ = action;
    action_t0_ = now_ms;
}

// Session state on the two antenna tips; it replaces every status icon on the pet.
//   idle: dim eye colour        connecting: blue breathing     listening: mint, follows the voice
//   thinking: blue, left/right ping-pong (also the Host's Ponder)
//   speaking: eye colour, follows its own voice   muted: dim red   error: red blink
// Host action receipts ride on top: running brightens, done blinks white, failed flashes red.
void Runtime::ComposeTips(const RuntimeInputs& in, uint64_t now) {
    const float t = static_cast<float>(now % 600000) / 1000.f;
    float l = mood_ == Mood::Asleep ? .05f : .3f, r = l;
    uint32_t rgb = 0;
    const bool pondering = in.phase == Phase::Thinking || (in.host_active && in.host_gesture == 2);
    const bool live = in.phase == Phase::Listening || in.phase == Phase::Thinking || in.phase == Phase::Responding;
    if (in.phase == Phase::Error) {
        l = r = std::fmod(t, .5f) < .25f ? .9f : .1f; rgb = kTipError;
    } else if (live && in.muted) {
        l = r = .22f; rgb = kTipMuted;
    } else if (pondering) {
        const float w = std::sin(t * kPi * 2 * 1.4f);
        l = .15f + .75f * Clamp(w, 0, 1); r = .15f + .75f * Clamp(-w, 0, 1); rgb = kTipThink;
    } else if (in.phase == Phase::Opening) {
        l = r = .25f + .45f * (.5f + .5f * std::sin(t * kPi * 2 / 1.2f)); rgb = kTipThink;
    } else if (in.phase == Phase::Listening) {
        l = r = Clamp(.3f + mic_level_ * 1.1f, 0, 1); rgb = kTipListen;
    } else if (in.phase == Phase::Responding) {
        l = r = Clamp(.3f + .7f * speech_level_, 0, 1);
    }
    const float at = static_cast<float>(now - action_t0_) / 1000.f;
    switch (action_) {
    case HostAction::Running:
        if (at < 15.f) { l = std::min(1.f, l + .25f); r = std::min(1.f, r + .25f); } else action_ = HostAction::None;
        break;
    case HostAction::Done:
        if (at < .22f) { l = r = 1.f; rgb = 0xFFFFFF; } else if (at >= .22f) action_ = HostAction::None;
        break;
    case HostAction::Failed:
        if (at < .8f) { if (std::fmod(at, .4f) < .2f) { l = r = 1.f; rgb = kTipError; } } else action_ = HostAction::None;
        break;
    default: break;
    }
    frame_.tip_l = l; frame_.tip_r = r; frame_.tip_rgb = rgb;
}

void Runtime::Emit(ParticleKind kind, int n) {
    const float cx = kFaceCx, cy = kFaceCy;
    for (int i = 0; i < n; ++i) {
        Particle* slot = nullptr;
        for (auto& q : frame_.particles) if (!q.alive) { slot = &q; break; }
        if (!slot) {
            slot = &frame_.particles[0];
            for (auto& q : frame_.particles) if (q.age / q.life > slot->age / slot->life) slot = &q;
        }
        Particle q; q.kind = kind; q.alive = true; q.x = cx; q.y = cy; q.phase = Rand(0, 6.28f);
        switch (kind) {
        case ParticleKind::Heart: q.x = cx + Rand(-70, 70); q.y = cy + Rand(10, 40); q.vx = Rand(-12, 12); q.vy = Rand(-55, -35); q.life = Rand(1.6f, 2.2f); q.size = Rand(.8f, 1.2f); break;
        case ParticleKind::Spark: { const float a = Rand(0, 6.28f), sp = Rand(60, 120); q.x = cx + Rand(-20, 20); q.y = cy - 10; q.vx = std::cos(a) * sp; q.vy = std::sin(a) * sp; q.life = Rand(.7f, 1.1f); q.size = Rand(.7f, 1.3f); break; }
        case ParticleKind::Note: q.x = cx + Rand(-110, 110); q.y = cy + Rand(30, 70); q.vx = Rand(-10, 10); q.vy = Rand(-45, -30); q.life = Rand(1.8f, 2.4f); break;
        case ParticleKind::Zz: q.x = cx + 70; q.y = cy - 40; q.vx = Rand(14, 22); q.vy = Rand(-26, -18); q.life = 2.4f; break;
        case ParticleKind::Sweat: q.x = cx + 98; q.y = cy - 46; q.vx = 2; q.vy = 8; q.g = 30; q.life = 1.4f; break;
        case ParticleKind::Dust: q.x = cx + Rand(-60, 60); q.y = 232; q.vx = Rand(-40, 40); q.vy = Rand(-14, -4); q.life = Rand(.6f, 1.f); q.size = Rand(.6f, 1.2f); break;
        }
        *slot = q;
    }
}

void Runtime::StepParticles(float dt) {
    for (auto& q : frame_.particles) {
        if (!q.alive) continue;
        q.age += dt; q.vy += q.g * dt; q.x += q.vx * dt; q.y += q.vy * dt;
        if (q.kind == ParticleKind::Spark) { q.vx *= .93f; q.vy *= .93f; }
        if (q.age > q.life) q.alive = false;
    }
}

float Runtime::Blink(uint64_t now, bool allowed, bool slow) {
    if (!allowed) { blink_start_ = 0; return 1; }
    if (now > blink_next_ && blink_start_ == 0) { blink_start_ = now; blink_dur_ = static_cast<uint64_t>(slow ? Rand(280, 380) : Rand(130, 170)); }
    if (!blink_start_) return 1;
    const float x = static_cast<float>(now - blink_start_) / static_cast<float>(blink_dur_);
    if (x >= 1) {
        blink_start_ = 0;
        const bool dbl = !slow && Rand(0, 1) < .15f;
        blink_next_ = now + static_cast<uint64_t>(dbl ? 160 : (slow ? Rand(4000, 8000) : Rand(2500, 6000)));
        return 1;
    }
    if (x < .38f) return 1 - Smooth(x / .38f);
    if (x < .52f) return .02f;
    return Smooth((x - .52f) / .48f);
}

void Runtime::UpdateMood(const RuntimeInputs& in) {
    const uint64_t now = in.now_ms;
    if (in.phase != Phase::Ambient && in.phase != Phase::System) {
        Interact(now);   // a conversation is company
        if (mood_ == Mood::Asleep || mood_ == Mood::Drowsy || mood_ == Mood::Bored) SetMood(Mood::Calm, now);
        return;
    }
    const uint64_t idle = now - last_interaction_, in_mood = now - mood_since_;
    switch (mood_) {
    case Mood::Happy: if (in_mood > 60000) SetMood(Mood::Content, now); break;
    case Mood::Content: if (in_mood > 120000) SetMood(Mood::Calm, now); break;
    case Mood::Curious: case Mood::Shy: if (in_mood > 20000) SetMood(Mood::Calm, now); break;
    case Mood::Grumpy: if (in_mood > 30000) SetMood(Mood::Calm, now); break;
    case Mood::Calm: if (idle > (in.dark ? kIdleToBoredDarkMs : kIdleToBoredMs)) SetMood(Mood::Bored, now); break;
    case Mood::Bored: if (idle > (in.dark ? kIdleToDrowsyDarkMs : kIdleToDrowsyMs)) SetMood(Mood::Drowsy, now); break;
    case Mood::Drowsy: if (idle > (in.dark ? kIdleToAsleepDarkMs : kIdleToAsleepMs)) SetMood(Mood::Asleep, now); break;
    case Mood::Asleep: break;
    }
}

const Frame& Runtime::Tick(const RuntimeInputs& in) {
    const uint64_t now = in.now_ms;
    if (first_tick_) {
        first_tick_ = false; last_ms_ = now; mood_since_ = now; last_interaction_ = now;
        next_act_ = now + 2500; next_glance_ = now + 30000; blink_next_ = now + 1800; next_twitch_ = now + 6000;
    }
    const float dt = Clamp(static_cast<float>(now - last_ms_) / 1000.f, 0, .05f);
    last_ms_ = now;
    phase_ = in.phase;
    if (in.phase != last_phase_) {
        // Entering or leaving a conversation pops; a turn changing hands inside one is a soft nudge,
        // since a quick bounce at the frame rate a busy audio path leaves reads as a jump.
        vel_.scale += InSession(last_phase_) && InSession(in.phase) ? .45f : 1.5f;
        if (in.phase == Phase::Responding) { aside_until_ = 0; next_aside_ = now + static_cast<uint64_t>(Rand(1600, 3000)); next_accent_ = now + 250; }
        last_phase_ = in.phase; phase_t0_ = now;
    }
    UpdateMood(in);
    mic_level_ += ((in.phase == Phase::Listening ? in.mic_level : 0.f) - mic_level_) * .35f;
    // Its own voice, measured against the voice's recent peak so the volume setting does not matter:
    // the peak sinks 3 dB/s, the top 24 dB below it map to 0..1; fast attack, slower release.
    float speech_target = 0;
    if (in.phase == Phase::Responding && in.speech_dbfs > -70.f) {
        speech_peak_db_ = std::max(in.speech_dbfs, speech_peak_db_ - dt * 3.f);
        speech_target = Clamp((in.speech_dbfs - (speech_peak_db_ - 24.f)) / 24.f, 0, 1);
    }
    speech_level_ += (speech_target - speech_level_) * (1 - std::exp(-dt / (speech_target > speech_level_ ? .035f : .14f)));
    // Emphasis: the envelope climbs well above its last dip. Eyes are the costliest part to redraw, so
    // they mark emphasis at most every 1.2 s; the antennae, redrawn every frame anyway, carry each syllable.
    speech_valley_ = std::min(speech_level_, speech_valley_ + dt * .8f);
    if (in.phase == Phase::Responding && !in.host_active && now >= next_accent_ && speech_level_ - speech_valley_ > .4f) {
        accent_t0_ = now; next_accent_ = now + 1200; speech_valley_ = speech_level_;
        // Emphasis: a small nod (up about 4 degrees and back), unless the head is turned aside.
        if (now >= aside_until_) QueueSpeakingClip(HeadKey{0, 0, 40, 160}, HeadKey{280, 0, 0, 140});
    }
    if (in.host_active && (!host_active_ || in.host_gesture != host_gesture_)) {
        host_t0_ = now;
        if (in.host_gesture == 5) Emit(ParticleKind::Spark, 8);
        if (reaction_.index >= 0) { reaction_.index = -1; activity_ = ""; }   // Host plans win; local never contends
    }
    host_active_ = in.host_active; host_gesture_ = in.host_gesture;

    // Reaction lifetime.
    if (reaction_.index >= 0) {
        const Reaction& r = kReactions[reaction_.index];
        const uint64_t rt = now - reaction_.start;
        while (reaction_.emit_index < r.emit_count && r.emits[reaction_.emit_index].at_ms <= rt) {
            Emit(r.emits[reaction_.emit_index].kind, r.emits[reaction_.emit_index].n); ++reaction_.emit_index;
        }
        bool periodic_ok = r.periodic_ms > 0;
        if (reaction_.index == kPadLive) periodic_ok = pad_down_ && pad_stroking_ && pad_dir_ >= 0;
        if (reaction_.index == kFaceLive) periodic_ok = face_stroking_;
        if (periodic_ok && now - reaction_.last_periodic > r.periodic_ms) { reaction_.last_periodic = now; Emit(r.periodic_kind, 1); }
        if (reaction_.end_at && now >= reaction_.end_at) {
            const bool after = r.has_after;
            const Mood next = r.after;
            const int ended = reaction_.index;
            reaction_.index = -1; activity_ = "";
            if (ended == kFaceLive) SetMood(Mood::Content, now);
            else if (after && in.phase == Phase::Ambient) SetMood(next, now);
        }
    }
    // Idle micro-behaviours.
    const int row = MoodActRow(mood_);
    const bool idle_ok = row >= 0 && in.phase == Phase::Ambient && reaction_.index < 0 && !in.host_active && !in.pointer && now >= card_until_;
    if (act_.index >= 0 && now - act_.start > act_.dur) { act_.index = -1; if (reaction_.index < 0) activity_ = ""; }
    if (idle_ok && act_.index < 0 && now > next_act_) {
        float total = 0;
        for (int i = 0; i < kActCount; ++i) total += kActWeights[row][i];
        float r = Rand(0, total); int pick = 0;
        for (int i = 0; i < kActCount; ++i) { r -= kActWeights[row][i]; if (r <= 0) { pick = i; break; } }
        act_ = ActState{};
        act_.index = pick; act_.start = now;
        act_.dur = static_cast<uint64_t>(Rand(kActDur[pick][0], kActDur[pick][1]));
        act_.x = Rand(-.6f, .6f); act_.y = Rand(-.3f, .22f); act_.s = Rand(0, 1) < .5f ? -1.f : 1.f;
        activity_ = kActNames[pick];
        if (pick == kDblBlink) { blink_next_ = now; blink_start_ = 0; }
        if (pick == kFlick) { twitch_side_ = static_cast<int>(act_.s); twitch_t0_ = now; }
        if (pick == kHum) Emit(ParticleKind::Note, 2);
        const bool near = now - last_interaction_ < 10000;
        next_act_ = now + act_.dur + static_cast<uint64_t>(Rand(kActGap[row][0], kActGap[row][1]) * (near ? .55f : 1.f));
    }
    // Sparse idle head glance (L4): head motion is expensive and audible, so rare.
    if (idle_ok && act_.index < 0 && (mood_ == Mood::Calm || mood_ == Mood::Content || mood_ == Mood::Curious) && now > next_glance_) {
        next_glance_ = now + static_cast<uint64_t>(Rand(25000, 60000));
        const int16_t yaw = static_cast<int16_t>(Rand(80, 150) * (Rand(0, 1) < .5f ? -1 : 1));
        pending_clip_ = HeadClip{};
        pending_clip_.count = 2; pending_clip_.priority = 4;
        pending_clip_.keys[0] = HeadKey{0, yaw, static_cast<int16_t>(Rand(-20, 60)), 180};
        pending_clip_.keys[1] = HeadKey{1600, 0, 0, 160};
        clip_pending_ = true;
    }
    // Ear twitch.
    if (idle_ok && now > next_twitch_) { twitch_side_ = Rand(0, 1) < .5f ? -1 : 1; twitch_t0_ = now; next_twitch_ = now + static_cast<uint64_t>(Rand(7000, 15000)); }

    // ---- compose the target pose ----
    Pose p;
    Glyph glyph = Glyph::None; Mouth mouth = Mouth::None; Emote emote = Emote::None;
    MoodRecipe(mood_, p, glyph, emote);
    if (mood_ == Mood::Drowsy) {   // occasional startle-awake
        const float ph = std::fmod(static_cast<float>(now - mood_since_) / 1000.f, 7.5f);
        if (ph > 6.3f && ph < 7.f) { p.lid_top_l = p.lid_top_r = .06f; p.scale = .12f; p.gaze_y = 0; p.glint = .6f; p.ear_l = p.ear_r = .6f; }
    }
    switch (in.phase) {
    case Phase::Opening: p.scale = .3f; p.glint = 1; p.ear_l = p.ear_r = 1; glyph = Glyph::None; break;
    case Phase::Listening: p = Pose{}; p.scale = .15f; p.round = .75f; p.glint = .9f; p.gaze_y = -.05f; p.ear_l = p.ear_r = 1; glyph = Glyph::None; emote = Emote::None; break;
    case Phase::Thinking: p = Pose{}; p.gaze_x = .3f + .1f * std::sin(now / 588.f); p.gaze_y = -.32f + .05f * std::sin(now / 435.f);
        p.lid_top_r = .15f; p.open_l = p.open_r = .88f; p.ear_l = .15f; p.ear_r = .85f; glyph = Glyph::None; emote = Emote::None; break;
    case Phase::Responding: {
        // Speaking face: the antennae lift and bob with each syllable, the eyes look at you, glance aside
        // now and then the way people do mid-sentence, and widen a little on emphasis. All small and soft,
        // so it reads smoothly at the frame rate the audio path leaves; the head stays still.
        p = Pose{}; p.ear_l = p.ear_r = .35f + .45f * speech_level_; glyph = Glyph::None; emote = Emote::None;
        if (now >= next_aside_) {
            aside_x_ = Rand(.1f, .2f) * (Rand(0, 1) < .5f ? -1.f : 1.f); aside_y_ = Rand(-.12f, .03f);
            const uint64_t dur = static_cast<uint64_t>(Rand(700, 1400));
            aside_until_ = now + dur;
            next_aside_ = aside_until_ + static_cast<uint64_t>(Rand(2200, 4800));
            // The head follows the glance a little (eyes lead, head follows) and comes back.
            if (!in.host_active)
                QueueSpeakingClip(HeadKey{0, static_cast<int16_t>(aside_x_ * 300), static_cast<int16_t>(-aside_y_ * 150), 140},
                                  HeadKey{static_cast<uint16_t>(dur), 0, 0, 140});
        }
        if (now < aside_until_) { p.gaze_x = aside_x_; p.gaze_y = aside_y_; }
        if (accent_t0_ && now - accent_t0_ < 300) {
            const float a = std::sin(kPi * static_cast<float>(now - accent_t0_) / 300.f);
            p.scale += .1f * a; p.squash -= .1f * a;
        }
        break;
    }
    case Phase::Error: p = Pose{}; p.open_l = .62f; p.lid_angle_l = -15; p.lid_top_l = .22f; p.open_r = .92f;
        p.brow_l = p.brow_r = .3f; p.brow_angle = -12; p.ear_l = -.5f; p.ear_r = -.2f; glyph = Glyph::None; emote = Emote::None; break;
    case Phase::System: p.ear_l = p.ear_r = .4f; break;
    case Phase::Ambient: break;
    }
    // Idle act blended in with its own envelope.
    if (act_.index >= 0 && reaction_.index < 0 && !in.host_active) {
        const float at = static_cast<float>(now - act_.start), d = static_cast<float>(act_.dur);
        Pose t = p;
        switch (act_.index) {
        case kLook: t.gaze_x = act_.x; t.gaze_y = act_.y; break;
        case kDblBlink: if (at > 230 && !act_.done2) { act_.done2 = true; blink_next_ = now; blink_start_ = 0; } break;
        case kTiltHead: t.roll = act_.s * .45f; t.scale = .08f; (act_.s < 0 ? t.open_l : t.open_r) = .85f; t.gaze_x = act_.s * .2f;
                        t.ear_l = act_.s < 0 ? .95f : .1f; t.ear_r = act_.s > 0 ? .95f : .1f; break;
        case kSquint: t.lid_bottom = .45f; t.squash = .12f; t.blush = .25f; break;
        case kDream: t.gaze_x = -.35f; t.gaze_y = -.6f; t.lid_top_l = t.lid_top_r = .12f; t.ear_l = .5f; t.ear_r = .1f; break;
        case kHop: {
            const float x = at / d;
            if (x < .2f) { t.squash = .32f; t.gaze_y = .12f; }
            else if (x < .45f) { t.squash = -.3f; t.gaze_y = -.45f; t.ear_l = t.ear_r = 1; }
            else { t.squash = .1f * (1 - x); }
            break;
        }
        case kWiggle: t.ear_l = std::sin(at / 85.f); t.ear_r = -std::sin(at / 85.f); break;
        case kHum: if (at > 1000 && !act_.done2) { act_.done2 = true; Emit(ParticleKind::Note, 1); } t.lid_bottom = .38f; t.squash = .08f; break;
        case kScan: t.gaze_x = .7f * std::sin(at / d * 2 * kPi); t.gaze_y = -.05f; break;
        case kYawn: t.mouth_size = at < 1400 ? .8f * Smooth(at / 500.f) : 0.f; t.lid_top_l = t.lid_top_r = .55f * Smooth(at / 700.f);
                    t.ear_l = t.ear_r = -.5f; t.squash = -.1f; if (at < 1400) mouth = Mouth::O; break;
        case kSneeze: if (at > 520 && !act_.done2) { act_.done2 = true; Emit(ParticleKind::Dust, 6); twitch_side_ = 1; twitch_t0_ = now; }
                      if (at < 500) { glyph = Glyph::Squeeze; t.squash = .2f; } else if (at < 760) { t.scale = .25f; t.squash = -.22f; t.ear_l = t.ear_r = 1; } break;
        default: break;
        }
        const float amt = kActRaw[act_.index] ? 1.f : Smooth(std::min({1.f, at / 180.f, (d - at) / 220.f}));
        Mix(p, t, amt);
    }
    // A finger on the screen is something real to look at.
    if (in.pointer && reaction_.index < 0 && !in.host_active) {
        p.gaze_x = Clamp((in.pointer_x - 160.f) / 110.f, -1, 1);
        p.gaze_y = Clamp((in.pointer_y - (kFaceCy - 8.f)) / 88.f, -1, 1);
        p.ear_l = std::max(p.ear_l, .35f - .4f * p.gaze_x); p.ear_r = std::max(p.ear_r, .35f + .4f * p.gaze_x);
    }
    // Local reaction.
    if (reaction_.index >= 0 && !in.host_active) {
        const Reaction& r = kReactions[reaction_.index];
        LiveState live{pad_down_, pad_stroking_, pad_pos_, pad_dir_, pad_stroking_ ? (now - pad_stroke_t0_) / 1000.f : 0.f,
                       face_down_, face_stroking_, face_x_, face_stroking_ ? (now - face_stroke_t0_) / 1000.f : 0.f};
        ReactionOut o;
        r.f(static_cast<float>(now - reaction_.start), p, o, live);
        if (o.glyph_set) glyph = o.glyph;
        if (o.mouth_set) mouth = o.mouth;
        if (o.emote_set) emote = o.emote;
        if (o.twitch && now - twitch_t0_ > 240) { twitch_side_ = o.twitch; twitch_t0_ = now; }
    }
    // Host face-core-1 gesture: eyes open, then blend toward the recipe.
    if (in.host_active) {
        Pose stable = p; stable.open_l = stable.open_r = 1;
        Pose target = stable; ReactionOut o;
        HostRecipe(in.host_gesture, target, o);
        p = stable; Mix(p, target, Clamp(in.host_amount, 0, 1) * .92f);
        if (o.mouth_set && in.host_amount > .2f) mouth = o.mouth;
        if (o.glyph_set && in.host_amount > .45f) glyph = o.glyph;
        if (o.emote_set && in.host_amount > .25f) emote = o.emote;
    }
    // Ears shiver with the voice while listening.
    if (in.phase == Phase::Listening && !in.host_active) { const float j = mic_level_ * .32f; p.ear_l += j * std::sin(now / 43.f); p.ear_r += j * std::sin(now / 47.f + 1.3f); }
    // Micro saccade.
    if (glyph == Glyph::None && !in.host_active && in.phase != Phase::System && mood_ != Mood::Asleep && in.phase != Phase::Thinking) {
        if (now > next_micro_) { micro_x_ = Rand(-.06f, .06f); micro_y_ = Rand(-.04f, .04f); next_micro_ = now + static_cast<uint64_t>(Rand(1200, 3400)); }
        p.gaze_x += micro_x_; p.gaze_y += micro_y_;
    }
    // In a conversation the gaze target eases rather than jumps: a big jump at the frame rate a busy
    // audio path leaves reads as a teleport. A finger on the screen still gets an immediate look.
    if (InSession(in.phase) && !in.pointer) {
        const float a = 1 - std::exp(-dt / .16f);
        gaze_tx_ += (p.gaze_x - gaze_tx_) * a; gaze_ty_ += (p.gaze_y - gaze_ty_) * a;
        p.gaze_x = gaze_tx_; p.gaze_y = gaze_ty_;
    } else {
        gaze_tx_ = pose_.gaze_x; gaze_ty_ = pose_.gaze_y;
    }
    p.gaze_x = Clamp(p.gaze_x, -1, 1); p.gaze_y = Clamp(p.gaze_y, -1, 1);
    p.ear_l = Clamp(p.ear_l, -1, 1); p.ear_r = Clamp(p.ear_r, -1, 1);
    p.scale = Clamp(p.scale, -1, 1); p.squash = Clamp(p.squash, -1, 1); p.round = Clamp(p.round, 0, 1);
    if (mouth == Mouth::None) p.mouth_size = 0;

    // ---- springs ----
    // Sub-stepped so a long frame (busy display task) cannot make a stiff spring diverge.
    const int substeps = std::max(1, static_cast<int>(std::ceil(dt / .008f)));
    const float h = dt / substeps;
    auto spring = [&](float& x, float& v, float target, float w, float z) {
        for (int k = 0; k < substeps; ++k) { v += (w * w * (target - x) - 2 * z * w * v) * h; x += v * h; }
    };
    float* cur = F(pose_); float* vel = F(vel_); const float* tgt = F(p);
    for (size_t i = 0; i < kPoseFloats; ++i) {
        const size_t gx = offsetof(Pose, gaze_x) / sizeof(float), gy = offsetof(Pose, gaze_y) / sizeof(float);
        const size_t el = offsetof(Pose, ear_l) / sizeof(float), er = offsetof(Pose, ear_r) / sizeof(float);
        const size_t sc = offsetof(Pose, scale) / sizeof(float), sq = offsetof(Pose, squash) / sizeof(float);
        const size_t g2x = offsetof(Pose, gaze_x2) / sizeof(float), g2y = offsetof(Pose, gaze_y2) / sizeof(float);
        if (i == g2x || i == g2y) continue;
        if (i == gx || i == gy) spring(cur[i], vel[i], tgt[i], 26, .55f);
        else if (i == el || i == er) spring(cur[i], vel[i], tgt[i], 17, .38f);
        else if (i == sc || i == sq) spring(cur[i], vel[i], tgt[i], 19, .5f);
        else cur[i] += (tgt[i] - cur[i]) * (1 - std::exp(-dt / .1f));
    }
    spring(pose_.gaze_x2, eye2_vx_, p.gaze_x, 22, .6f);
    spring(pose_.gaze_y2, eye2_vy_, p.gaze_y, 22, .6f);
    const float kick = (std::fabs(vel_.gaze_x) + std::fabs(vel_.gaze_y)) * .04f;
    if (kick > .08f) { vel_.ear_l -= kick; vel_.ear_r -= kick; }

    // ---- glyph transition: squash to a line, swap, expand ----
    if (glyph != glyph_target_) { glyph_from_ = glyph_cur_; glyph_target_ = glyph; glyph_t0_ = now; }
    const float gph = static_cast<float>(now - glyph_t0_);
    const bool changing = glyph_from_ != glyph_target_;
    Glyph show; float kq;
    if (changing && gph < 90) { show = glyph_from_; kq = 1 - Smooth(gph / 90.f); }
    else { glyph_cur_ = glyph_target_; show = glyph_cur_; kq = changing && gph < 220 ? Smooth((gph - 90) / 130.f) : 1.f; }
    if (mouth != Mouth::None) mouth_cur_ = mouth; else if (pose_.mouth_size < .03f) mouth_cur_ = Mouth::None;
    if (emote != emote_cur_) { emote_cur_ = emote; emote_t0_ = now; }

    // ---- frame ----
    const bool blink_ok = show == Glyph::None && !in.host_active && mood_ != Mood::Asleep && in.phase != Phase::System;
    frame_.blink = Blink(now, blink_ok, mood_ == Mood::Bored || mood_ == Mood::Drowsy);
    frame_.pose = pose_;
    // Stepped breathing: eight levels, one step per 600 ms, so a resting face is not redrawn every frame.
    // Not during a conversation: each step redraws both eyes, a periodic stall while audio is busy,
    // and the antennae and the speaking face carry the life there.
    if (!InSession(in.phase)) {
        const float breath_level = std::round(4 * std::sin((now / 600) * .6f * 2 * kPi / 4.8f)) / 4.f;
        frame_.pose.scale += .056f * breath_level;
    }
    frame_.glyph = show; frame_.glyph_k = kq;
    frame_.mouth = mouth_cur_;
    frame_.emote = emote_cur_; frame_.emote_age = (now - emote_t0_) / 1000.f;
    if (now < bubble_until_) {
        frame_.bubble = bubble_;
        frame_.bubble_alpha = Clamp(std::min((now - bubble_t0_) / 200.f, (bubble_until_ - now) / 350.f), 0, 1);
    } else { frame_.bubble = Bubble::None; frame_.bubble_alpha = 0; }
    ComposeTips(in, now);
    const float tp = (now - twitch_t0_) / 220.f;
    const float tw = (tp >= 0 && tp < 1) ? std::sin(tp * kPi * 4) * 12 * (1 - tp) : 0.f;
    frame_.twitch_l = twitch_side_ < 0 || twitch_side_ == 0 ? tw : 0;
    frame_.twitch_r = twitch_side_ > 0 || twitch_side_ == 0 ? tw : 0;
    frame_.card += ((now < card_until_ ? 1.f : 0.f) - frame_.card) * (1 - std::exp(-dt / .09f));
    frame_.face_shift = frame_.card;
    night_mix_ += ((in.dark ? 1.f : 0.f) - night_mix_) * (1 - std::exp(-dt / .7f));
    frame_.eye_rgb = MixRgb(0xF2ECDF, 0xFFC46B, night_mix_);
    frame_.bg_rgb = MixRgb(0x121316, 0x17110A, night_mix_);
    // Particles: mood-driven ambience and motion.
    if (in.phase == Phase::Ambient && reaction_.index < 0 && now > next_periodic_) {
        if (mood_ == Mood::Happy) { Emit(ParticleKind::Heart, 1); next_periodic_ = now + 2600; }
        else if (mood_ == Mood::Asleep) { Emit(ParticleKind::Zz, 1); next_periodic_ = now + 1500; }
        else next_periodic_ = now + 500;
    }
    StepParticles(dt);
    return frame_;
}

void Runtime::Led(uint64_t now, uint8_t out[36]) const {
    std::memset(out, 0, 36);
    const uint32_t skin = frame_.eye_rgb;
    auto set = [&](int i, uint32_t c, float a) {
        a = Clamp(a, 0, 1) * .6f;   // brightness cap
        out[i * 3 + 0] = static_cast<uint8_t>(((c >> 16) & 255) * a);
        out[i * 3 + 1] = static_cast<uint8_t>(((c >> 8) & 255) * a);
        out[i * 3 + 2] = static_cast<uint8_t>((c & 255) * a);
    };
    auto all = [&](uint32_t c, float a) { for (int i = 0; i < 12; ++i) set(i, c, a); };
    auto bottom_up = [](int side, int i) { return side * 6 + (5 - i); };
    LedMode mode = LedMode::Off;
    float t = 0;
    if (phase_ == Phase::Listening) mode = LedMode::Vu;
    else if (phase_ == Phase::Thinking) mode = LedMode::Comet;
    else if (phase_ == Phase::Error) mode = LedMode::AmberSteady;
    else if (charging_) mode = LedMode::Charge;
    else if (battery_low_) mode = LedMode::AmberDim;
    t = (now % 100000) / 1000.f;
    if (reaction_.index >= 0) {
        const Reaction& r = kReactions[reaction_.index];
        if (r.led != LedMode::Off || phase_ == Phase::Ambient) mode = r.led;
        t = (now - reaction_.start) / 1000.f;
        if (reaction_.index == kPadLive || reaction_.index == kFaceLive) {
            // Live touch: the live recipe picks the colour (pink with the fur, amber against).
            mode = (reaction_.index == kPadLive && pad_stroking_ && pad_dir_ < 0) ? LedMode::AmberPulse :
                   ((pad_stroking_ || face_stroking_) ? LedMode::PinkBreath : LedMode::SkinFlash);
        }
    }
    if (host_active_) { mode = LedMode::Gesture; t = (now - host_t0_) / 1000.f; }
    if (mood_ == Mood::Asleep && phase_ == Phase::Ambient) mode = LedMode::Off;
    switch (mode) {
    case LedMode::Vu: { const float n = mic_level_ * 6; for (int s = 0; s < 2; ++s) for (int i = 0; i < 6; ++i) set(bottom_up(s, i), 0x7CE0A8, i < n ? .3f + .5f * Clamp(n - i, 0, 1) : .08f); break; }
    case LedMode::Comet: { const float pos = std::fmod(t / 1.2f, 1.f) * 7.5f - .5f; for (int s = 0; s < 2; ++s) for (int i = 0; i < 6; ++i) set(bottom_up(s, i), 0x6FA8FF, std::max(0.f, 1 - std::fabs(i - pos) * .7f) * .6f); break; }
    case LedMode::FlashWarm: all(0xFFC98A, .6f * std::exp(-t / .6f)); break;
    case LedMode::SkinFlash: all(skin, .55f * std::exp(-t / .45f)); break;
    case LedMode::PinkBreath: all(0xFF9DB5, .14f + .22f * (.5f + .5f * std::sin(t * 2 * kPi / 2.2f))); break;
    case LedMode::Amber2: all(0xFFB547, (t < .12f || (t > .26f && t < .38f)) ? .65f : 0.f); break;
    case LedMode::AmberPulse: all(0xFFB547, .07f + .2f * (.5f + .5f * std::sin(t * 2 * kPi / 3))); break;
    case LedMode::AmberDim: all(0xFFB547, .05f + .06f * (.5f + .5f * std::sin(t * 2 * kPi / 4))); break;
    case LedMode::AmberSteady: all(0xFFB547, .16f); break;
    case LedMode::BlueBreath: all(0x6FA8FF, .08f + .24f * (.5f + .5f * std::sin(t * 2 * kPi / 3))); break;
    case LedMode::SkinBreath: all(skin, .08f + .2f * (.5f + .5f * std::sin(t * 2 * kPi / 1.6f))); break;
    case LedMode::Marquee: { static const int ring[12] = {5, 4, 3, 2, 1, 0, 6, 7, 8, 9, 10, 11}; const float hd = std::fmod(t * 11, 12.f);
        for (int j = 0; j < 12; ++j) { const float d = std::fmod(hd - j + 12, 12.f); set(ring[j], skin, d < 4 ? .55f * (1 - d / 4) : 0); } break; }
    case LedMode::Welcome: { static const int ring[12] = {5, 4, 3, 2, 1, 0, 6, 7, 8, 9, 10, 11}; const float hd = std::fmod(t * 9, 14.f), fade = std::exp(-t / 2.2f);
        for (int j = 0; j < 12; ++j) { const float d = std::fmod(hd - j + 14, 14.f); set(ring[j], 0xFFB06A, (d < 5 ? .6f * (1 - d / 5) : .05f) * fade + .04f); } break; }
    case LedMode::Beat: { const float b = std::max(0.f, std::sin(t * 2 * kPi)); for (int s = 0; s < 2; ++s) for (int i = 0; i < 6; ++i) set(bottom_up(s, i), skin, i < b * 6 ? .2f + .4f * b : .04f); break; }
    case LedMode::Charge: { const float fill = std::fmod(t, 4.f) / 4 * 6.5f; for (int s = 0; s < 2; ++s) for (int i = 0; i < 6; ++i) set(bottom_up(s, i), 0x8EE6A6, i < fill ? .12f + .18f * Clamp(fill - i, 0, 1) : .03f); break; }
    case LedMode::Gesture: all(kHostLed[host_gesture_ & 7], .5f * std::exp(-t / .6f) + .04f); break;
    case LedMode::Off: default: break;
    }
}

}  // namespace eidolon::pet
