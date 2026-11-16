#pragma once

// Local head clips as smooth paths (docs/跨系统/StackChan自然转动分析-20261116.md, E1).
// Pure and header-only: the body plays a path on the servos, and the runtime evaluates
// the same path to keep the eyes ahead of the head. No ESP-IDF, no LVGL.
//
// A clip's keys are poses with arrival times. The path starts at the head's pose when
// the clip starts, passes through every key and stops at the last one:
//   - Each segment is a quintic Hermite curve with zero acceleration at both ends.
//     With zero velocities that is the min-jerk curve (10t^3 - 15t^4 + 6t^5), the
//     same curve the Reachy Mini SDK moves with by default.
//   - A key where an axis turns back, holds, or ends the clip has zero velocity on
//     that axis. A key the axis passes through gets the smaller of its two neighbouring
//     average speeds, so the motion carries through it instead of stopping. That
//     velocity choice keeps every segment monotone (no overshoot past a key) and its
//     peak speed at most 1.875 times its average speed.
//   - A segment that is too fast for its key's speed cap is stretched until its peak
//     speed fits, and every later key moves later by the same amount.
// Angles are deci-degrees relative to the rest pose (yaw 0 = centred, dpitch 0 = rest,
// + = up). Time is milliseconds from the start of the clip.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "pet_face.h"

namespace eidolon::pet {

// Min-jerk progress for tau in [0, 1].
inline float MinJerk(float tau) {
    if (tau <= 0) return 0;
    if (tau >= 1) return 1;
    return tau * tau * tau * (10 + tau * (-15 + 6 * tau));
}

// The quickest a segment may cover delta_dd and still peak at max_dps (min-jerk peaks at
// 1.875 times its average speed). max_dps = 0 leaves the authored timing alone.
inline float HeadSegmentMinMs(float delta_dd, uint16_t max_dps) {
    if (max_dps == 0) return 0;
    return 187.5f * std::fabs(delta_dd) / static_cast<float>(max_dps);
}

// Where the head is when a clip starts: pose in deci-degrees, velocity in deci-degrees
// per millisecond (non-zero only when the clip takes over from a path still moving).
struct HeadPathStart {
    float yaw = 0, pitch = 0;
    float vyaw = 0, vpitch = 0;
};

// Range the path stays inside (deci-degrees relative to rest). Keys outside it are
// moved to the edge, so the path never leaves it either.
struct HeadPathLimits {
    float yaw_lo = -1280, yaw_hi = 1280;
    float pitch_lo = -1000, pitch_hi = 1000;
};

struct HeadSample {
    float yaw = 0, pitch = 0;      // deci-degrees
    float vyaw = 0, vpitch = 0;    // deci-degrees per millisecond
    bool done = true;              // past the last key
};

namespace head_path_detail {

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Knots {
    size_t n = 0;                          // knot 0 is the start, knot i + 1 is key i
    float t[kMaxHeadKeys + 1] = {};
    float yaw[kMaxHeadKeys + 1] = {};
    float pitch[kMaxHeadKeys + 1] = {};
};

inline Knots Build(const HeadClip& clip, const HeadPathStart& from, const HeadPathLimits& lim) {
    Knots k;
    const size_t count = clip.count < kMaxHeadKeys ? clip.count : kMaxHeadKeys;
    k.n = count + 1;
    k.t[0] = 0;
    k.yaw[0] = from.yaw;
    k.pitch[0] = from.pitch;
    float authored_prev = 0;
    for (size_t i = 0; i < count; ++i) {
        const HeadKey& key = clip.keys[i];
        const float yaw = Clamp(static_cast<float>(key.yaw_dd), lim.yaw_lo, lim.yaw_hi);
        const float pitch = Clamp(static_cast<float>(key.dpitch_dd), lim.pitch_lo, lim.pitch_hi);
        const float dy = std::fabs(yaw - k.yaw[i]), dp = std::fabs(pitch - k.pitch[i]);
        const float authored = static_cast<float>(key.at_ms) - authored_prev;
        authored_prev = static_cast<float>(key.at_ms);
        float seg = authored > 0 ? authored : 0;
        const float need = HeadSegmentMinMs(dy > dp ? dy : dp, key.max_dps);
        if (seg < need) seg = need;
        k.t[i + 1] = k.t[i] + seg;
        k.yaw[i + 1] = yaw;
        k.pitch[i + 1] = pitch;
    }
    return k;
}

// Velocity at interior knot i for one axis (see the header comment).
inline float PassThrough(const float* p, const float* t, size_t i) {
    const float t0 = t[i] - t[i - 1], t1 = t[i + 1] - t[i];
    if (t0 <= 0 || t1 <= 0) return 0;
    const float s0 = (p[i] - p[i - 1]) / t0, s1 = (p[i + 1] - p[i]) / t1;
    if (s0 == 0 || s1 == 0 || (s0 > 0) != (s1 > 0)) return 0;
    return s0 > 0 ? (s0 < s1 ? s0 : s1) : (s0 > s1 ? s0 : s1);
}

// Quintic Hermite with zero end accelerations: position and velocity at tau.
inline void Hermite(float p0, float p1, float v0, float v1, float T, float tau, float& pos, float& vel) {
    const float t2 = tau * tau, t3 = t2 * tau, t4 = t3 * tau, t5 = t4 * tau;
    const float h5 = 10 * t3 - 15 * t4 + 6 * t5;            // p1 (and 1 - h5 for p0)
    const float h1 = tau - 6 * t3 + 8 * t4 - 3 * t5;         // v0 * T
    const float h4 = -4 * t3 + 7 * t4 - 3 * t5;              // v1 * T
    pos = p0 + (p1 - p0) * h5 + T * (v0 * h1 + v1 * h4);
    const float d5 = 30 * t2 - 60 * t3 + 30 * t4;
    const float d1 = 1 - 18 * t2 + 32 * t3 - 15 * t4;
    const float d4 = -12 * t2 + 28 * t3 - 15 * t4;
    vel = (p1 - p0) * d5 / T + v0 * d1 + v1 * d4;
}

}  // namespace head_path_detail

// When the path reaches its last key.
inline float HeadPathEndMs(const HeadClip& clip, const HeadPathStart& from, const HeadPathLimits& lim = {}) {
    const auto k = head_path_detail::Build(clip, from, lim);
    return k.t[k.n - 1];
}

// When the path reaches key `index` (its stretched arrival time).
inline float HeadPathKeyMs(const HeadClip& clip, const HeadPathStart& from, size_t index,
                           const HeadPathLimits& lim = {}) {
    const auto k = head_path_detail::Build(clip, from, lim);
    return index + 1 < k.n ? k.t[index + 1] : k.t[k.n - 1];
}

// The head's pose and velocity t_ms into the clip.
inline HeadSample SampleHeadPath(const HeadClip& clip, const HeadPathStart& from, float t_ms,
                                 const HeadPathLimits& lim = {}) {
    using namespace head_path_detail;
    const Knots k = Build(clip, from, lim);
    HeadSample s;
    if (k.n < 2 || t_ms >= k.t[k.n - 1]) {
        s.yaw = k.yaw[k.n - 1];
        s.pitch = k.pitch[k.n - 1];
        s.done = true;
        return s;
    }
    if (t_ms < 0) t_ms = 0;
    size_t i = 1;
    while (i < k.n - 1 && t_ms >= k.t[i]) ++i;   // segment from knot i - 1 to knot i
    const float T = k.t[i] - k.t[i - 1];
    s.done = false;
    if (T <= 0) {
        s.yaw = k.yaw[i];
        s.pitch = k.pitch[i];
        return s;
    }
    const float tau = (t_ms - k.t[i - 1]) / T;
    const float vy0 = i == 1 ? from.vyaw : PassThrough(k.yaw, k.t, i - 1);
    const float vp0 = i == 1 ? from.vpitch : PassThrough(k.pitch, k.t, i - 1);
    const float vy1 = i + 1 < k.n ? PassThrough(k.yaw, k.t, i) : 0;
    const float vp1 = i + 1 < k.n ? PassThrough(k.pitch, k.t, i) : 0;
    Hermite(k.yaw[i - 1], k.yaw[i], vy0, vy1, T, tau, s.yaw, s.vyaw);
    Hermite(k.pitch[i - 1], k.pitch[i], vp0, vp1, T, tau, s.pitch, s.vpitch);
    s.yaw = Clamp(s.yaw, lim.yaw_lo, lim.yaw_hi);
    s.pitch = Clamp(s.pitch, lim.pitch_lo, lim.pitch_hi);
    return s;
}

}  // namespace eidolon::pet
