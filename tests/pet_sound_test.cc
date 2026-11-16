// Host tests for the pet's chirps (main/eidolon/pet/pet_sound.*): clean, audible on a
// small speaker, and saying what the face says (the prosody conventions people read).
#include "eidolon/pet/pet_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace eidolon::pet;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

constexpr int kRate = 16000;   // the device's output rate
constexpr int kAna = 32000;    // pitch analysis: finer lag steps at high pitch

std::vector<int16_t> RenderAll(Chirp c, int rate, float volume, size_t block) {
    ChirpRenderer r(c, rate, volume);
    std::vector<int16_t> pcm, buf(block);
    for (int guard = 0; !r.done() && guard < 100000; ++guard) {
        const size_t n = r.Render(buf.data(), block);
        pcm.insert(pcm.end(), buf.begin(), buf.begin() + n);
    }
    CHECK(r.done());
    return pcm;
}
std::vector<int16_t> R(Chirp c) { return RenderAll(c, kRate, 1.f, 256); }
int Peak(const std::vector<int16_t>& p) { int m = 0; for (int16_t v : p) m = std::max(m, std::abs(static_cast<int>(v))); return m; }
double Rms(const std::vector<int16_t>& p) { double s = 0; for (int16_t v : p) s += double(v) * v; return p.empty() ? 0 : std::sqrt(s / p.size()); }

// Pitch per 20 ms frame (10 ms hop) by normalised autocorrelation, 280..1700 Hz; 0 = unvoiced.
std::vector<float> Pitch(const std::vector<int16_t>& p, int rate) {
    const int frame = rate / 50, hop = rate / 100, lo = rate / 1700, hi = rate / 280;
    std::vector<float> out;
    for (size_t at = 0; at + frame + hi < p.size(); at += hop) {
        double e = 0; for (int i = 0; i < frame; ++i) e += double(p[at + i]) * p[at + i];
        if (std::sqrt(e / frame) < 400) { out.push_back(0); continue; }
        std::vector<double> r(hi + 1, 0);
        double best = 0;
        for (int lag = lo; lag <= hi; ++lag) {
            double xy = 0, yy = 0;
            for (int i = 0; i < frame; ++i) { xy += double(p[at + i]) * p[at + i + lag]; yy += double(p[at + i + lag]) * p[at + i + lag]; }
            r[lag] = xy / std::sqrt(e * yy + 1);
            best = std::max(best, r[lag]);
        }
        float f = 0;
        if (best > .55)   // the shortest lag close to the best: avoids octave-down picks
            for (int lag = lo; lag <= hi; ++lag) if (r[lag] >= .9 * best && (lag == hi || r[lag] >= r[lag + 1])) {
                double l = lag;   // parabolic refinement
                if (lag > lo && lag < hi) { const double d = r[lag - 1] - 2 * r[lag] + r[lag + 1]; if (d < 0) l += .5 * (r[lag - 1] - r[lag + 1]) / d; }
                f = float(rate / l); break;
            }
        out.push_back(f);
    }
    return out;
}
std::vector<float> Voiced(Chirp c) { auto v = Pitch(RenderAll(c, kAna, 1.f, 256), kAna); v.erase(std::remove(v.begin(), v.end(), 0.f), v.end()); return v; }
float Median(std::vector<float> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
float Head(const std::vector<float>& v) { return Median({v.begin(), v.begin() + std::max<size_t>(1, v.size() * 2 / 5)}); }
float Tail(const std::vector<float>& v) { return Median({v.end() - std::max<size_t>(1, v.size() / 4), v.end()}); }
float Mean(const std::vector<float>& v) { double s = 0; for (float f : v) s += f; return v.empty() ? 0 : float(s / v.size()); }
float Max(const std::vector<float>& v) { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }

// Share of the energy below 250 Hz (4th-order low-pass): what the StackChan speaker cannot play.
double BassShare(const std::vector<int16_t>& p, int rate) {
    const double w0 = 2 * M_PI * 250 / rate, alpha = std::sin(w0) / (2 * .707), a0 = 1 + alpha;
    const double b0 = (1 - std::cos(w0)) / 2 / a0, b1 = (1 - std::cos(w0)) / a0, a1 = -2 * std::cos(w0) / a0, a2 = (1 - alpha) / a0;
    double s1[4] = {}, s2[4] = {}, lo = 0, all = 0;
    for (int16_t v : p) {
        double x = v;
        all += x * x;
        for (int k = 0; k < 2; ++k) {   // two cascaded sections
            double* st = k ? s2 : s1;
            const double y = b0 * x + b1 * st[0] + b0 * st[1] - a1 * st[2] - a2 * st[3];
            st[1] = st[0]; st[0] = x; st[3] = st[2]; st[2] = y; x = y;
        }
        lo += x * x;
    }
    return all > 0 ? lo / all : 0;
}

void TestEveryChirpIsShortAudibleAndClean() {
    for (int i = 1; i < static_cast<int>(Chirp::kCount); ++i) {
        const Chirp c = static_cast<Chirp>(i);
        const uint32_t ms = ChirpDurationMs(c);
        CHECK(ms >= 80 && ms <= 800);                          // a chirp, not a jingle
        const auto pcm = R(c);
        CHECK(std::abs(static_cast<long>(pcm.size()) - static_cast<long>(ms * 16)) <= 16);
        const int peak = Peak(pcm);
        CHECK(peak <= 32767 * 3 / 10);                        // soft-limited: never clips at any volume
        CHECK(Rms(pcm) > 1200);                               // clearly audible
        CHECK(std::abs(pcm.front()) < 600 && std::abs(pcm.back()) < 600);   // no click at either end
        // No click inside: loudness in 1 ms windows never leaps (a click is the level jumping
        // from near silence to full within a millisecond; every fade here takes 2 ms or more).
        int step = 0; double prev = 0;
        for (size_t at = 0; at + 16 <= pcm.size(); at += 16) {
            double e = 0; for (size_t k = at; k < at + 16; ++k) e += double(pcm[k]) * pcm[k];
            const double rms = std::sqrt(e / 16);
            step = std::max(step, static_cast<int>(std::fabs(rms - prev))); prev = rms;
        }
        CHECK(step < 3800);
        CHECK(BassShare(pcm, kRate) < .03);                   // above the small speaker's floor
        if (failures) std::fprintf(stderr, "  %s: %u ms peak %d rms %.0f step %d bass %.3f\n", ChirpName(c), ms, peak,
                                   Rms(pcm), step, BassShare(pcm, kRate));
    }
    CHECK(ChirpDurationMs(Chirp::None) == 0);
    ChirpRenderer none(Chirp::None, kRate, 1); int16_t b[8];
    CHECK(none.done() && none.Render(b, 8) == 0);
}

void TestBlocksDoNotChangeTheSound() {
    for (int i = 1; i < static_cast<int>(Chirp::kCount); ++i) {
        const Chirp c = static_cast<Chirp>(i);
        CHECK(RenderAll(c, kRate, 1.f, 100000) == RenderAll(c, kRate, 1.f, 37));
        CHECK(RenderAll(c, kRate, 1.f, 256) == R(c));   // and always the same (fixed noise seed)
    }
}

void TestVolumeAndRate() {
    const auto loud = R(Chirp::Bip), soft = RenderAll(Chirp::Bip, kRate, .45f, 256);
    CHECK(std::abs(Peak(soft) - Peak(loud) * .45) < 40);
    const auto hi = RenderAll(Chirp::Bip, 24000, 1.f, 256);
    CHECK(std::abs(static_cast<long>(hi.size()) - static_cast<long>(ChirpDurationMs(Chirp::Bip) * 24)) <= 24);
    auto at = [](int rate) { auto v = Pitch(RenderAll(Chirp::Sad, rate, 1.f, 256), rate); v.erase(std::remove(v.begin(), v.end(), 0.f), v.end()); return Mean(v); };
    CHECK(std::fabs(at(24000) - at(kAna)) < at(kAna) * .05f);   // same pitch at another rate
}

// The face and the voice must say the same thing.
void TestTheVoiceSaysWhatTheFaceSays() {
    auto report = [](Chirp c) {
        const auto v = Voiced(c);
        std::printf("  %-9s head %4.0f Hz  tail %4.0f Hz  mean %4.0f  max %4.0f  voiced %zu\n", ChirpName(c), Head(v), Tail(v), Mean(v), Max(v), v.size());
    };
    for (int i = 1; i < static_cast<int>(Chirp::kCount); ++i) report(static_cast<Chirp>(i));
    // A question ends rising: the "?" over the shoulder, a knock, "where did you go?", waking up.
    for (Chirp c : {Chirp::Huh, Chirp::Knock, Chirp::Hmm, Chirp::Wake}) { const auto v = Voiced(c); CHECK(Tail(v) > Head(v) * 1.15f); }
    // Relief, sadness, a yawn and a huff sink at the end.
    for (Chirp c : {Chirp::Sad, Chirp::Phew, Chirp::Yawn, Chirp::Hmph}) { const auto v = Voiced(c); CHECK(Tail(v) < Head(v) * .87f); }
    // Startled and tapped-at-the-front jump high.
    CHECK(Max(Voiced(Chirp::Eep)) > 1100 && Max(Voiced(Chirp::Bip)) > 950);
    // Happy sits far above sad; annoyance sits low.
    CHECK(Mean(Voiced(Chirp::Peekaboo)) > 1.6f * Mean(Voiced(Chirp::Sad)));
    CHECK(Mean(Voiced(Chirp::Grumble)) < 600 && Mean(Voiced(Chirp::Hmph)) < 480);
    // No two reactions sound the same.
    for (int i = 1; i < static_cast<int>(Chirp::kCount); ++i)
        for (int j = i + 1; j < static_cast<int>(Chirp::kCount); ++j) CHECK(R(static_cast<Chirp>(i)) != R(static_cast<Chirp>(j)));
}
}  // namespace

int main() {
    TestEveryChirpIsShortAudibleAndClean();
    TestBlocksDoNotChangeTheSound();
    TestVolumeAndRate();
    TestTheVoiceSaysWhatTheFaceSays();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pet sound tests passed\n");
    return 0;
}
