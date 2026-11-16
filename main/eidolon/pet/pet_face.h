#pragma once

// Pet face vocabulary (device-internal, never on the wire). The Host still sends
// face-core-1 gesture IDs; this file is how StackChan draws them, plus the local
// life that needs no session: moods, glances, reactions to touch and motion.
// Pure data: no ESP-IDF, no LVGL.

#include <array>
#include <cstddef>
#include <cstdint>

namespace eidolon::pet {

// FacePose v2 (docs/stackchan-desk-pet-design.md §4.3). Every field interpolates.
struct Pose {
    float gaze_x = 0, gaze_y = 0;     // -1..1 (left eye; right eye follows on its own spring)
    float gaze_x2 = 0, gaze_y2 = 0;   // right eye, filled by the runtime
    float open_l = 1, open_r = 1;     // 0..1
    float scale = 0, squash = 0;      // -1..1
    float round = .72f;               // 0..1
    float lid_top_l = 0, lid_top_r = 0;
    float lid_angle_l = 0, lid_angle_r = 0;   // degrees, + = inner side lower
    float lid_bottom = 0;             // 0..1, smile-shaped lower lid
    float glint = .75f, blush = 0, mouth_size = 0;
    float brow_l = 0, brow_r = 0, brow_angle = 0;
    float ear_l = 0, ear_r = 0;       // 1 perked .. -1 flattened
    float cross = 0, roll = 0;
};

enum class Glyph : uint8_t { None, Heart, Star, Spiral, Happy, Content, Squeeze, Line, X };
enum class Mouth : uint8_t { None, O, W, V, Wave };
enum class Emote : uint8_t { None, Bang, Question, Anger, Dots };
enum class ParticleKind : uint8_t { Heart, Spark, Note, Zz, Sweat, Dust };
enum class Bubble : uint8_t { None, Moon, Bell, Battery, Bolt, Thermo, Note };
enum class Mood : uint8_t { Calm, Content, Happy, Curious, Bored, Drowsy, Asleep, Grumpy, Shy };
// The conversation phase as the shared UI model reports it.
enum class Phase : uint8_t { Ambient, Opening, Listening, Thinking, Responding, Error, System };
enum class LedMode : uint8_t {
    Off, Vu, Comet, FlashWarm, SkinFlash, PinkBreath, Amber2, AmberPulse, AmberDim,
    AmberSteady, BlueBreath, SkinBreath, Marquee, Welcome, Beat, Charge, Gesture
};

constexpr int kParticles = 12;
struct Particle {
    ParticleKind kind = ParticleKind::Heart;
    bool alive = false;
    float x = 0, y = 0, vx = 0, vy = 0, g = 0, age = 0, life = 1, size = 1, phase = 0;
};

// One keyframe of a local head clip, in deci-degrees relative to the body's rest pose
// (yaw 0 = centred; dpitch 0 = rest, positive = up). The body places it in its own
// mechanical range, so the pet never needs to know where rest sits on a given head.
// at_ms is when the head arrives at the key; max_dps caps the peak speed of the move
// into it (the move is stretched to fit). Two equal keys in a row hold the pose.
// The path between keys is pet_head_path.h.
struct HeadKey {
    uint16_t at_ms = 0;
    int16_t yaw_dd = 0;
    int16_t dpitch_dd = 0;
    uint16_t max_dps = 30;
};

// Face centre on the 320x240 pet screen. The status bar owns the top 32 px, so the
// face sits a little below the screen's middle; particles and pointer gaze use it too.
constexpr float kFaceCx = 160.f;
constexpr float kFaceCy = 134.f;
constexpr size_t kMaxHeadKeys = 10;
struct HeadClip {
    std::array<HeadKey, kMaxHeadKeys> keys{};
    size_t count = 0;
    // 1 posture before the microphone opens, 2 reflex, 3 attention (speaking), 4 idle.
    // Server commands always win.
    uint8_t priority = 0;
    // The last key is where the head should stay (a new heading, a mood's posture, the
    // listening posture): the body does not take it home afterwards. Sits in what was
    // padding, so the clip keeps its size.
    bool hold = false;
};

// Everything the renderer needs for one frame.
struct Frame {
    Pose pose;
    float blink = 1;          // multiplies open_l/open_r
    Glyph glyph = Glyph::None;
    float glyph_k = 1;        // 0 = squashed to a line during a glyph change
    Mouth mouth = Mouth::None;
    Emote emote = Emote::None;
    float emote_age = 0;      // seconds since the emote appeared (pop-in)
    Bubble bubble = Bubble::None;
    float bubble_alpha = 0;
    // The antenna tips carry the session state on the pet (no status icon): brightness
    // per tip 0..1 and their colour (0 = the eye colour).
    float tip_l = .3f, tip_r = .3f;
    uint32_t tip_rgb = 0;
    float twitch_l = 0, twitch_r = 0;   // extra ear rotation, degrees
    float card = 0;           // 0..1 glance card slide
    float face_shift = 0;     // 0..1 face lifts while the card is up
    uint32_t eye_rgb = 0xF2ECDF, bg_rgb = 0x121316;
    std::array<Particle, kParticles> particles{};
};

const char* MoodName(Mood mood);

}  // namespace eidolon::pet
