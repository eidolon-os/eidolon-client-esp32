#pragma once

// The pet's local life: mood state machine, face composition, springs, blinks,
// idle micro-behaviours, reactions to local senses, particles and LED patterns.
// Single-threaded by design (the display task owns it). Pure C++, host-tested.
//
// Layering (design §4.5): mood base -> conversation phase -> gaze (idle act /
// finger) -> one transient layer (Host face-core-1 gesture wins over a local
// reaction) -> micro saccade -> blink. Local reactions never produce receipts and
// never contend with Host plans: the view stops feeding a reaction while a Host
// plan is playing.

#include <cstdint>

#include "pet_face.h"
#include "pet_sense.h"
#include "pet_sound.h"

namespace eidolon::pet {

struct RuntimeInputs {
    uint64_t now_ms = 0;
    Phase phase = Phase::Ambient;
    float mic_level = 0;          // 0..1, recent capture RMS while listening
    bool host_active = false;     // a face-core-1 step is on screen
    uint8_t host_gesture = 0;     // expression::Gesture index (attend..attention)
    float host_amount = 0;        // 0..1 envelope of that step
    bool pointer = false;         // a finger is on the screen
    float pointer_x = 160, pointer_y = 120;
    bool dark = false;            // ambient light: room is dark
    bool muted = false;           // the Owner muted the microphone
    float speech_dbfs = -100;     // the companion's own playback level while it speaks (-100 = silent)
};

// Receipt of a Host action (face plan or head motion), shown on the antenna tips.
enum class HostAction : uint8_t { None, Running, Done, Failed };

class Runtime {
public:
    explicit Runtime(uint32_t seed = 0x5EED1234u);

    // Local senses (touch pad, IMU, light, servo readback, power).
    void OnSense(const SenseEvent& event);
    // Owner presence from the Host (body.presence.set).
    void OnPresence(bool awake, uint64_t now_ms);
    // A tap on the face that opens a conversation: perk up.
    void OnScreenTap(uint64_t now_ms);
    // Live finger on the face: down/moving (stroking when it travels), then up.
    void OnScreenStroke(bool down, float x, float travelled_px, uint64_t now_ms);
    // charging = external power connected. The first call only records the state.
    void OnPower(bool charging, int battery_pct, uint64_t now_ms);
    void ForceMood(Mood mood, uint64_t now_ms);
    void OnHostAction(HostAction action, uint64_t now_ms);

    const Frame& Tick(const RuntimeInputs& in);
    // A local head clip that wants to play now. The caller decides whether motion
    // is allowed (permission, capture) and drops it otherwise.
    bool TakeHeadClip(HeadClip& out);
    // The chirp the latest reaction asked for (taken once); the caller decides if it may play.
    bool TakeChirp(Chirp& out);
    // 12 RGB triples (left column top->bottom, then right column top->bottom).
    void Led(uint64_t now_ms, uint8_t out_rgb[36]) const;

    Mood mood() const { return mood_; }
    // What drives the transient layer right now (for diagnostics), "" when idle.
    const char* activity() const { return activity_; }
    float speech_level() const { return speech_level_; }   // 0..1, relative to the voice's recent peak
    // True while a local reaction or idle act is animating (redraw needed).
    bool animating() const;
    // Card currently requested (double tap on the head).
    bool card_requested(uint64_t now_ms) const { return now_ms < card_until_; }

private:
    struct ReactionState {
        int index = -1;
        uint64_t start = 0, end_at = 0;
        uint8_t emit_index = 0;
        uint64_t last_periodic = 0;
    };
    struct ActState {
        int index = -1;
        uint64_t start = 0, dur = 0;
        float x = 0, y = 0, s = 1;
        bool done2 = false;
    };

    void StartReaction(int index, uint64_t now_ms);
    void SetMood(Mood mood, uint64_t now_ms);
    void Interact(uint64_t now_ms) { last_interaction_ = now_ms; }
    void UpdateMood(const RuntimeInputs& in);
    void Emit(ParticleKind kind, int n);
    void StepParticles(float dt);
    float Blink(uint64_t now_ms, bool allowed, bool slow);
    uint32_t Next();
    float Rand(float a, float b);

    Frame frame_;
    Pose pose_;              // smoothed
    Pose vel_;               // spring velocities (only spring fields used)
    float eye2_vx_ = 0, eye2_vy_ = 0;
    uint32_t rng_;
    uint64_t last_ms_ = 0;
    bool first_tick_ = true;

    Mood mood_ = Mood::Calm;
    uint64_t mood_since_ = 0, last_interaction_ = 0;
    Phase last_phase_ = Phase::Ambient;

    ReactionState reaction_;
    ActState act_;
    uint64_t next_act_ = 2500, next_glance_ = 30000, next_micro_ = 0, next_periodic_ = 0;
    float micro_x_ = 0, micro_y_ = 0;
    // Blink.
    uint64_t blink_next_ = 1800, blink_start_ = 0, blink_dur_ = 150;
    // Glyph transition.
    Glyph glyph_cur_ = Glyph::None, glyph_from_ = Glyph::None, glyph_target_ = Glyph::None;
    uint64_t glyph_t0_ = 0;
    Mouth mouth_cur_ = Mouth::None;
    Emote emote_cur_ = Emote::None;
    uint64_t emote_t0_ = 0;
    // Ear twitch.
    int twitch_side_ = 0;
    uint64_t twitch_t0_ = 0, next_twitch_ = 6000;
    // Live inputs.
    bool face_down_ = false, face_stroking_ = false;
    float face_x_ = 160;
    uint64_t face_stroke_t0_ = 0;
    bool pad_down_ = false, pad_stroking_ = false;
    float pad_pos_ = 0;
    int pad_dir_ = 0;
    uint64_t pad_t0_ = 0, pad_stroke_t0_ = 0;
    // Bubble / card / power.
    Bubble bubble_ = Bubble::None;
    uint64_t bubble_t0_ = 0, bubble_until_ = 0, card_until_ = 0;
    bool charging_ = false, battery_low_ = false, power_known_ = false;
    float night_mix_ = 0, mic_level_ = 0;
    Phase phase_ = Phase::Ambient;
    bool host_active_ = false;
    uint8_t host_gesture_ = 0;
    uint64_t host_t0_ = 0;
    HostAction action_ = HostAction::None;
    uint64_t action_t0_ = 0;
    void ComposeTips(const RuntimeInputs& in, uint64_t now);
    // Conversation: a calm, speech-led face. The head stays still while it listens, thinks and speaks.
    uint64_t phase_t0_ = 0;
    float speech_peak_db_ = -30, speech_level_ = 0, speech_valley_ = 0;
    uint64_t accent_t0_ = 0, next_accent_ = 0, next_aside_ = 0, aside_until_ = 0;
    float aside_x_ = 0, aside_y_ = 0, gaze_tx_ = 0, gaze_ty_ = 0;
    HeadClip pending_clip_;
    bool clip_pending_ = false;
    Chirp pending_chirp_ = Chirp::None;
    void QueueSpeakingClip(const HeadKey& a, const HeadKey& b);
    const char* activity_ = "";
};

}  // namespace eidolon::pet
