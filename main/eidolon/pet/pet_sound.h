#pragma once

// The pet's voice: short synthesized chirps for its local reactions. No audio assets:
// each chirp is a few vowel-like syllables (formant synthesis) rendered on the fly. Pure logic, host-tested
// (tests/pet_sound_test.cc); the device plays them only outside a conversation, where
// the microphone is closed and the speaker is free.

#include <cstddef>
#include <cstdint>

namespace eidolon::pet {

enum class Chirp : uint8_t {
    None,
    Bip,       // tapped at the front: a bright "bip?" looking up at the hand
    Boing,     // tapped in the middle: squashed, springs back
    Huh,       // tapped at the back: "huh?" over the shoulder
    Purr,      // stroked with the fur, or a hand resting on the head: content
    Grumble,   // stroked against the fur, or the head turned by hand: ruffled
    Hmph,      // patted too much
    Pop,       // double tap: the glance card pops up
    Eep,       // picked up: startled
    Phew,      // put down: relief
    Dizzy,     // shaken
    Knock,     // a knock on the desk: "who's there?"
    Peekaboo,  // uncovered
    Hmm,       // covered
    Yawn,      // lights off
    Wake,      // lights on
    Giggle,    // a face close up: shy
    Yum,       // charger in
    Sad,       // battery low
    kCount
};

const char* ChirpName(Chirp chirp);
uint32_t ChirpDurationMs(Chirp chirp);

// Renders one chirp as mono 16-bit PCM, block by block, so the player needs only a
// small buffer. `volume` 0..1 scales the whole chirp (night plays it softer).
class ChirpRenderer {
public:
    ChirpRenderer(Chirp chirp, int sample_rate, float volume);
    // Writes up to `max` samples; returns how many (0 once done()).
    size_t Render(int16_t* out, size_t max);
    bool done() const;

private:
    Chirp chirp_;
    int rate_;
    float volume_;
    size_t seg_ = 0;          // current syllable
    size_t pos_ = 0;          // sample within it
    float phase_ = 0;         // source phase 0..1, continuous across syllables (no clicks)
    float vib_phase_ = 0, trem_phase_ = 0;
    uint32_t noise_ = 0x2545F491u;   // breath noise; fixed seed so a chirp always sounds the same
    float c1_[5] = {}, c2_[5] = {}, c3_[5] = {};   // formant band-pass coefficients
    float z1_[2] = {}, z2_[2] = {}, z3_[2] = {};   // and their state
    float g2_ = 0, g3_ = 0;
};

}  // namespace eidolon::pet
