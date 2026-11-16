#include "pet_sound.h"

#include <algorithm>
#include <cmath>

// A small creature's voice: each chirp is one to four syllables. A syllable is a buzzy
// source (band-limited saw, optionally mixed with breath noise) shaped by three formant
// resonators into a vowel, with its own pitch glide and envelope. Meaning rides on the
// prosody, the way people read a pet: a question ends rising, contentment rises then
// settles, annoyance is low, rough and falling, surprise jumps high, sadness sinks.
// Everything sits above ~300 Hz: the StackChan speaker has no bass.

namespace eidolon::pet {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kLevel = .27f;     // peak level at full volume (soft-limited, never clips)
constexpr int kCoefEvery = 16;     // samples between formant coefficient updates

enum Vowel : uint8_t { kI, kE, kA, kO, kU, kM };   // kM: a closed-mouth hum ("n", "m")
struct Formants { float f1, f2, f3, g2, g3; };
// Adult formants scaled ~1.25x for a small vocal tract; g2/g3 weight F2/F3 against F1.
constexpr Formants kVowel[] = {
    {340, 2700, 3400, .9f, .45f},   // i
    {520, 2300, 3200, .8f, .35f},   // e
    {900, 1500, 3000, .7f, .25f},   // a
    {600, 1050, 2900, .6f, .18f},   // o
    {420, 950, 2800, .45f, .12f},   // u
    {320, 1300, 2700, .25f, .08f},  // m / n
};

enum Glide : uint8_t {
    kEven,     // even glide (in log pitch)
    kEarly,    // most of the move early, then settles (a call, a happy lift)
    kLate,     // starts slow, moves late (a question's rise, finished while still loud)
};

// One syllable. f0 0 = a rest. breath 1 = whisper only ("f", "h").
struct Syl {
    uint16_t ms;
    float f0a, f0b;
    Vowel va = kA, vb = kA;
    float amp = 1;
    Glide glide = kEven;
    float vib_hz = 0, vib = 0;    // vibrato: rate and depth (fraction of pitch)
    float breath = 0;             // noise mixed into the source
    float growl_hz = 0;           // amplitude flutter: a purr, a growl
    uint8_t attack_ms = 6, release_ms = 40;
};

struct Shape { const Syl* syls; size_t count; };

constexpr Syl R(uint16_t ms) { return Syl{ms, 0, 0}; }

// tap front: flinch, then up at the hand with a glint. "pi!"
constexpr Syl kBip[] = {{130, 720, 1100, kI, kI, 1, kEarly, 0, 0, .12f, 0, 3, 50}};
// tap middle: ears squashed down, then spring up. "mu-pa!"
constexpr Syl kBoing[] = {{90, 430, 360, kU, kU, .75f, kEven, 0, 0, 0, 0, 8, 20}, R(20),
                          {140, 520, 840, kA, kA, 1, kEarly, 0, 0, .05f, 0, 3, 60}};
// tap back: a glance over the shoulder, "?". "n-e?"
constexpr Syl kHuh[] = {{70, 480, 470, kM, kM, .6f, kEven, 0, 0, 0, 0, 8, 15},
                        {180, 500, 840, kE, kE, 1, kLate, 0, 0, 0, 0, 4, 55}};
// stroked with the fur, a hand resting: happy eyes, blush. "nya~a" with a purr.
constexpr Syl kPurr[] = {{110, 520, 640, kM, kA, .85f, kEarly, 0, 0, 0, 0, 10, 0},
                         {390, 640, 540, kA, kO, .9f, kEven, 5, .015f, .05f, 26, 0, 140}};
// stroked against the fur, head turned by hand: squeezed eyes, brow down. "nn-gya!"
constexpr Syl kGrumble[] = {{90, 400, 390, kM, kM, .7f, kEven, 0, 0, .05f, 34, 8, 10},
                            {180, 620, 420, kE, kA, 1, kEarly, 0, 0, .18f, 30, 3, 60}};
// patted too much: anger mark, eyes narrowed. "hm! hmph!"
constexpr Syl kHmph[] = {{80, 470, 390, kM, kM, .9f, kEven, 0, 0, .3f, 0, 3, 25}, R(60),
                         {140, 450, 340, kM, kU, 1, kEarly, 0, 0, .3f, 0, 3, 60}};
// double tap: the glance card is shown. "pa-pon!"
constexpr Syl kPop[] = {{70, 600, 640, kA, kA, .9f, kEven, 0, 0, .06f, 0, 3, 20}, R(30),
                        {150, 800, 900, kO, kO, 1, kEarly, 0, 0, 0, 0, 3, 80}};
// picked up: startled, then nervous. "hya!?"
constexpr Syl kEep[] = {{150, 600, 1250, kA, kI, 1, kEarly, 0, 0, .15f, 0, 2, 10},
                        {130, 1250, 1150, kI, kI, .8f, kEven, 9, .05f, 0, 0, 4, 70}};
// put down: relief. "fu-wa~"
constexpr Syl kPhew[] = {{120, 600, 600, kU, kU, .55f, kEven, 0, 0, 1, 0, 15, 20},
                         {270, 640, 420, kU, kA, .85f, kEven, 0, 0, .35f, 0, 10, 120}};
// shaken: spiral eyes. "wa-wa-wa-wa~"
constexpr Syl kDizzy[] = {{90, 640, 600, kU, kA, .9f}, R(20), {90, 580, 540, kU, kA, .9f}, R(20),
                          {90, 540, 500, kU, kA, .9f}, R(20), {240, 500, 430, kU, kA, .85f, kEven, 7, .08f, 0, 0, 6, 110}};
// a knock on the desk: looks up. "n? n?"
constexpr Syl kKnock[] = {{90, 520, 660, kM, kE, .8f, kLate, 0, 0, 0, 0, 6, 25}, R(70),
                          {110, 560, 780, kE, kE, .9f, kLate, 0, 0, 0, 0, 4, 50}};
// uncovered: peekaboo, sparkles. "pi-ka-a!"
constexpr Syl kPeekaboo[] = {{70, 760, 780, kI, kI, .9f, kEven, 0, 0, .08f, 0, 3, 15}, R(15),
                             {80, 860, 880, kA, kA, .95f, kEven, 0, 0, .05f, 0, 3, 15}, R(15),
                             {180, 1000, 1180, kA, kE, 1, kEarly, 6, .02f, 0, 0, 3, 90}};
// covered: "where did you go?" "a-re?"
constexpr Syl kHmm[] = {{110, 520, 500, kA, kA, .8f, kEven, 0, 0, 0, 0, 8, 25}, R(20),
                        {220, 470, 760, kE, kE, .9f, kLate, 0, 0, 0, 0, 6, 45}};
// lights off: a yawn, then sleepy. "fua~aa..."
constexpr Syl kYawn[] = {{80, 560, 560, kU, kU, .5f, kEven, 0, 0, 1, 0, 15, 10},
                         {200, 520, 760, kU, kA, .8f, kEarly, 0, 0, .3f, 0, 10, 0},
                         {420, 760, 400, kA, kO, .7f, kEven, 4, .01f, .25f, 0, 0, 200}};
// lights on: squints awake. "n-n?"
constexpr Syl kWake[] = {{120, 420, 440, kM, kM, .6f, kEven, 0, 0, 0, 0, 12, 20},
                         {160, 460, 700, kM, kE, .85f, kLate, 0, 0, 0, 0, 6, 60}};
// a face close up: cross-eyed and shy. "e-he-he"
constexpr Syl kGiggle[] = {{70, 800, 860, kE, kE, .95f, kEven, 0, 0, .15f, 0, 3, 20}, R(25),
                           {60, 760, 820, kE, kE, .9f, kEven, 0, 0, .18f, 0, 3, 20}, R(25),
                           {70, 720, 780, kE, kE, .85f, kEven, 0, 0, .18f, 0, 3, 40}};
// charger in: dinner. "nom-nom, na~!"
constexpr Syl kYum[] = {{80, 480, 470, kO, kM, .8f, kEven, 0, 0, 0, 0, 5, 20}, R(30),
                        {80, 490, 480, kO, kM, .8f, kEven, 0, 0, 0, 0, 5, 20}, R(40),
                        {200, 600, 820, kA, kA, 1, kEarly, 0, 0, 0, 0, 4, 90}};
// battery low: hungry and sleepy. "u-u~"
constexpr Syl kSad[] = {{160, 560, 520, kU, kU, .8f, kEven, 0, 0, .05f, 0, 10, 30}, R(40),
                        {320, 500, 370, kU, kO, .7f, kEven, 5, .02f, .1f, 0, 8, 160}};

template <size_t N> constexpr Shape S(const Syl (&s)[N]) { return {s, N}; }

Shape ShapeOf(Chirp c) {
    switch (c) {
    case Chirp::Bip: return S(kBip);
    case Chirp::Boing: return S(kBoing);
    case Chirp::Huh: return S(kHuh);
    case Chirp::Purr: return S(kPurr);
    case Chirp::Grumble: return S(kGrumble);
    case Chirp::Hmph: return S(kHmph);
    case Chirp::Pop: return S(kPop);
    case Chirp::Eep: return S(kEep);
    case Chirp::Phew: return S(kPhew);
    case Chirp::Dizzy: return S(kDizzy);
    case Chirp::Knock: return S(kKnock);
    case Chirp::Peekaboo: return S(kPeekaboo);
    case Chirp::Hmm: return S(kHmm);
    case Chirp::Yawn: return S(kYawn);
    case Chirp::Wake: return S(kWake);
    case Chirp::Giggle: return S(kGiggle);
    case Chirp::Yum: return S(kYum);
    case Chirp::Sad: return S(kSad);
    default: return {nullptr, 0};
    }
}

float GlideAt(Glide g, float x) {
    switch (g) {
    case kEarly: return 1 - (1 - x) * (1 - x);
    case kLate: return x * x;
    default: return x;
    }
}

// PolyBLEP correction for a naive saw: removes most aliasing at the wrap.
float PolyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

// RBJ band-pass, 0 dB peak gain.
void BandPass(float f, float bw, int rate, float c[5]) {
    f = std::min(f, .45f * rate);
    const float w0 = 2 * kPi * f / rate, q = std::max(.7f, f / bw);
    const float alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
    c[0] = alpha / a0; c[1] = 0; c[2] = -alpha / a0; c[3] = -2 * std::cos(w0) / a0; c[4] = (1 - alpha) / a0;
}

}  // namespace

const char* ChirpName(Chirp chirp) {
    static const char* const kNames[] = {"none", "bip", "boing", "huh", "purr", "grumble", "hmph", "pop", "eep",
                                         "phew", "dizzy", "knock", "peekaboo", "hmm", "yawn", "wake", "giggle",
                                         "yum", "sad"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Chirp::kCount), "one name per chirp");
    const size_t i = static_cast<size_t>(chirp);
    return i < static_cast<size_t>(Chirp::kCount) ? kNames[i] : "?";
}

uint32_t ChirpDurationMs(Chirp chirp) {
    const Shape s = ShapeOf(chirp);
    uint32_t ms = 0;
    for (size_t i = 0; i < s.count; ++i) ms += s.syls[i].ms;
    return ms;
}

ChirpRenderer::ChirpRenderer(Chirp chirp, int sample_rate, float volume)
    : chirp_(chirp), rate_(sample_rate > 0 ? sample_rate : 16000), volume_(std::clamp(volume, 0.f, 1.f)) {}

bool ChirpRenderer::done() const { return seg_ >= ShapeOf(chirp_).count; }

size_t ChirpRenderer::Render(int16_t* out, size_t max) {
    const Shape shape = ShapeOf(chirp_);
    const float dt = 1.f / static_cast<float>(rate_);
    size_t n = 0;
    while (n < max && seg_ < shape.count) {
        const Syl& s = shape.syls[seg_];
        const size_t len = std::max<size_t>(1, static_cast<size_t>(s.ms) * rate_ / 1000);
        // Fades: as written, but never a hard edge next to silence (start, end, a rest).
        // Between two voiced syllables a zero attack/release means legato.
        const bool after_rest = seg_ == 0 || shape.syls[seg_ - 1].f0a <= 0;
        const bool before_rest = seg_ + 1 == shape.count || shape.syls[seg_ + 1].f0a <= 0;
        size_t att = static_cast<size_t>(s.attack_ms) * rate_ / 1000;
        size_t rel = static_cast<size_t>(s.release_ms) * rate_ / 1000;
        if (after_rest) att = std::max<size_t>(att, rate_ / 500);    // >= 2 ms
        if (before_rest) rel = std::max<size_t>(rel, rate_ / 250);   // >= 4 ms
        while (n < max && pos_ < len) {
            float v = 0;
            if (s.f0a > 0) {
                const float x = static_cast<float>(pos_) / static_cast<float>(len);
                float f0 = s.f0a * std::pow(s.f0b / s.f0a, GlideAt(s.glide, x));
                if (pos_ % kCoefEvery == 0) {
                    const Formants& a = kVowel[s.va];
                    const Formants& b = kVowel[s.vb];
                    const float k = x * x * (3 - 2 * x);
                    // At a high pitch the harmonics are sparse; formants widen with it (as in a
                    // child's voice) so a harmonic always lands in each one and the vowel stays loud.
                    BandPass(a.f1 + (b.f1 - a.f1) * k, std::max(110.f, .5f * f0), rate_, c1_);
                    BandPass(a.f2 + (b.f2 - a.f2) * k, std::max(150.f, .6f * f0), rate_, c2_);
                    BandPass(a.f3 + (b.f3 - a.f3) * k, std::max(260.f, .7f * f0), rate_, c3_);
                    g2_ = a.g2 + (b.g2 - a.g2) * k; g3_ = a.g3 + (b.g3 - a.g3) * k;
                }
                if (s.vib_hz > 0) { vib_phase_ += 2 * kPi * s.vib_hz * dt; f0 *= 1 + s.vib * std::sin(vib_phase_); }
                const float step = f0 * dt;
                phase_ += step;
                if (phase_ >= 1) phase_ -= 1;
                const float saw = 2 * phase_ - 1 - PolyBlep(phase_, step);
                noise_ = noise_ * 1664525u + 1013904223u;
                const float white = static_cast<float>(static_cast<int32_t>(noise_)) / 2147483648.f;
                const float src = saw * (1 - s.breath) + white * s.breath * 1.6f;
                auto run = [src](const float c[5], float z[2]) {   // transposed direct form II
                    const float y = c[0] * src + z[0];
                    z[0] = c[1] * src - c[3] * y + z[1];
                    z[1] = c[2] * src - c[4] * y;
                    return y;
                };
                float y = run(c1_, z1_) + g2_ * run(c2_, z2_) + g3_ * run(c3_, z3_);
                float env = s.amp;
                if (att && pos_ < att) env *= static_cast<float>(pos_) / static_cast<float>(att);
                if (rel && pos_ + rel > len) env *= static_cast<float>(len - pos_) / static_cast<float>(rel);
                if (s.growl_hz > 0) { trem_phase_ += 2 * kPi * s.growl_hz * dt; env *= .6f + .4f * std::sin(trem_phase_); }
                // Soft limit: formant peaks vary by vowel; tanh keeps every chirp under the level.
                v = std::tanh(2.2f * y * env) * kLevel * volume_;
            } else {
                z1_[0] = z1_[1] = z2_[0] = z2_[1] = z3_[0] = z3_[1] = 0;   // a rest clears the resonators
            }
            out[n++] = static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
            ++pos_;
        }
        if (pos_ >= len) { ++seg_; pos_ = 0; }
    }
    return n;
}

}  // namespace eidolon::pet
