#include "eidolon/pet/pet_motion.h"

#include <algorithm>
#include <cmath>

namespace eidolon::pet {

namespace {
using Vec = MotionRecognizer::Vec;

constexpr float kLowPassAlpha = 0.1f;  // ~100 ms at 100 Hz
constexpr uint32_t kSamplePeriodMs = 10;
constexpr uint32_t kMaxStepMs = 50;    // a gap in the stream must not count as deviation time
constexpr uint32_t kCoverageSlackMs = 20;

Vec Sub(const Vec& a, const Vec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Norm(const Vec& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

float AngleDeg(const Vec& a, const Vec& b)
{
    const float na = Norm(a), nb = Norm(b);
    if (na < 1e-3f || nb < 1e-3f) return 0;
    const float c = std::clamp((a.x * b.x + a.y * b.y + a.z * b.z) / (na * nb), -1.0f, 1.0f);
    return std::acos(c) * 180.0f / 3.14159265f;
}

SenseEvent Event(SenseKind kind, uint64_t t, float value = 0)
{
    SenseEvent e;
    e.kind = kind;
    e.t_ms = t;
    e.value = value;
    return e;
}
}  // namespace

MotionRecognizer::MotionRecognizer(const MotionConfig& config) : cfg_(config) {}

void MotionRecognizer::SuppressUntil(uint64_t t_ms)
{
    suppress_until_ = std::max(suppress_until_, t_ms);
    if (suppress_until_ > last_t_) {
        knock_phase_ = KnockPhase::Idle;
        rest_valid_ = false;
        deviation_ms_ = 0;
        tilt_pending_ = false;
    }
}

void MotionRecognizer::Update(const ImuSample& s, std::vector<SenseEvent>& out)
{
    const Vec a{s.ax, s.ay, s.az};
    const float mag = Norm(a);
    const uint32_t dt = started_ && s.t_ms > last_t_
                            ? static_cast<uint32_t>(std::min<uint64_t>(s.t_ms - last_t_, kMaxStepMs))
                            : kSamplePeriodMs;
    const bool suppressed = s.t_ms < suppress_until_;

    // The knock detector compares against the rest pose and the samples
    // before this one, so it runs before the sample enters the window.
    UpdateKnock(s, a, suppressed, out);

    Entry e;
    e.t = s.t_ms;
    e.a = a;
    e.mag = mag;
    Push(e);
    if (!started_) {
        lp_ = a;
        started_ = true;
    } else {
        lp_.x += kLowPassAlpha * (a.x - lp_.x);
        lp_.y += kLowPassAlpha * (a.y - lp_.y);
        lp_.z += kLowPassAlpha * (a.z - lp_.z);
    }
    last_t_ = s.t_ms;
    last_magnitude_ = mag;

    Vec mean;
    const bool still = ComputeStill(s.t_ms, mean);
    if (still && !still_) still_since_ = s.t_ms;
    still_ = still;
    if (still) still_mean_ = mean;
    const uint64_t still_ms = still ? s.t_ms - still_since_ : 0;

    if (suppressed) {
        rest_valid_ = false;
        deviation_ms_ = 0;
        tilt_pending_ = false;
    }

    if (!level_valid_ && !suppressed && still && still_ms >= cfg_.level_learn_ms) {
        level_ = mean;
        level_valid_ = true;
    }

    // Put down / rest pose. Being still in a hand for this long is rare
    // enough (tremor breaks the std test) to count as set down.
    if (held_) {
        if (still && still_ms >= cfg_.put_down_still_ms) {
            held_ = false;
            rest_ = mean;
            rest_valid_ = true;
            out.push_back(Event(SenseKind::PutDown, s.t_ms));
        }
    } else if (!suppressed && still && still_ms >= cfg_.put_down_still_ms) {
        rest_ = mean;  // follows slow drift and nudges while resting
        rest_valid_ = true;
    }

    // Picked up: acceleration alone is a knock or a push, rotation alone is
    // someone tilting it on the desk; lifting gives both.
    if (!suppressed && std::fabs(mag - 1.0f) > cfg_.pick_deviation_g) {
        deviation_ms_ += dt;
        last_deviation_t_ = s.t_ms;
    } else if (s.t_ms - last_deviation_t_ > cfg_.pick_window_ms) {
        deviation_ms_ = 0;
    }
    if (!held_ && rest_valid_ && !suppressed && deviation_ms_ >= cfg_.pick_deviation_ms) {
        const float angle = AngleDeg(lp_, rest_);
        if (angle > cfg_.pick_angle_deg) {
            held_ = true;
            deviation_ms_ = 0;
            tilt_pending_ = false;
            out.push_back(Event(SenseKind::PickedUp, s.t_ms, angle));
        }
    }

    // Shaken: once per episode (until the variance falls well below the
    // threshold), and never twice within the cooldown.
    const float shake_std = ShakeStd(s.t_ms);
    if (shake_std > cfg_.shake_std_g) {
        if (!shake_high_) {
            shake_high_ = true;
            shake_since_ = s.t_ms;
        }
    } else if (shake_std < cfg_.shake_std_g * 0.6f) {
        shake_high_ = false;
        shake_reported_ = false;
    }
    if (shake_high_ && !shake_reported_ && s.t_ms - shake_since_ >= cfg_.shake_sustain_ms &&
        (!shake_ever_ || s.t_ms - last_shake_t_ >= cfg_.shake_cooldown_ms)) {
        shake_reported_ = true;
        shake_ever_ = true;
        last_shake_t_ = s.t_ms;
        out.push_back(Event(SenseKind::Shaken, s.t_ms, shake_std));
    }

    // Tilted / Level, judged only at rest on the desk.
    if (level_valid_ && !held_ && still && !suppressed) {
        const float angle = AngleDeg(mean, level_);
        if (!tilted_) {
            if (angle <= cfg_.tilt_deg) {
                tilt_pending_ = false;
            } else if (!tilt_pending_) {
                tilt_pending_ = true;
                tilt_since_ = s.t_ms;
            } else if (s.t_ms - tilt_since_ >= cfg_.tilt_hold_ms) {
                tilted_ = true;
                tilt_pending_ = false;
                out.push_back(Event(SenseKind::Tilted, s.t_ms, angle));
            }
        } else if (angle < cfg_.level_deg) {
            tilted_ = false;
            out.push_back(Event(SenseKind::Level, s.t_ms, angle));
        }
    } else {
        tilt_pending_ = false;
    }
}

void MotionRecognizer::UpdateKnock(const ImuSample& s, const Vec& a, bool suppressed,
                                   std::vector<SenseEvent>& out)
{
    if (suppressed || held_ || !rest_valid_) {
        knock_phase_ = KnockPhase::Idle;
        return;
    }
    const float d = Norm(Sub(a, rest_));
    switch (knock_phase_) {
    case KnockPhase::Idle:
        if (d > cfg_.knock_peak_g && QuietBefore(s.t_ms)) {
            knock_phase_ = KnockPhase::Impulse;
            knock_start_ = knock_last_above_ = s.t_ms;
            knock_peak_ = d;
        }
        break;
    case KnockPhase::Impulse:
        if (d > cfg_.knock_peak_g * 0.5f) {
            knock_peak_ = std::max(knock_peak_, d);
            knock_last_above_ = s.t_ms;
            // Too long for a knock: a push, a drop or the start of a lift.
            // Each sample stands for one period, so N samples last N periods.
            if (s.t_ms - knock_start_ + kSamplePeriodMs >= cfg_.knock_max_ms) knock_phase_ = KnockPhase::Idle;
        } else {
            knock_phase_ = KnockPhase::Settle;
            knock_settle_end_ = s.t_ms + cfg_.knock_settle_ms;
            knock_sum_ = a;
            knock_n_ = 1;
        }
        break;
    case KnockPhase::Settle:
        if (d > cfg_.knock_peak_g) {
            knock_phase_ = KnockPhase::Idle;  // still moving: not a clean impulse
            break;
        }
        knock_sum_.x += a.x;
        knock_sum_.y += a.y;
        knock_sum_.z += a.z;
        ++knock_n_;
        if (s.t_ms >= knock_settle_end_) {
            const float n = static_cast<float>(knock_n_);
            const Vec after{knock_sum_.x / n, knock_sum_.y / n, knock_sum_.z / n};
            // Same attitude before and after: the body was struck, not moved.
            if (AngleDeg(after, rest_) < cfg_.knock_attitude_deg &&
                std::fabs(Norm(after) - Norm(rest_)) < 0.1f) {
                out.push_back(Event(SenseKind::Knock, knock_start_, knock_peak_));
            }
            knock_phase_ = KnockPhase::Idle;
        }
        break;
    }
}

void MotionRecognizer::Push(const Entry& e)
{
    ring_[ring_head_] = e;
    ring_head_ = (ring_head_ + 1) % kRing;
    if (ring_size_ < kRing) ++ring_size_;
}

const MotionRecognizer::Entry& MotionRecognizer::Back(size_t i) const
{
    return ring_[(ring_head_ + kRing - 1 - i) % kRing];
}

bool MotionRecognizer::ComputeStill(uint64_t t, Vec& mean) const
{
    // Two passes in float: the ESP32-S3 FPU is single precision, and E[x^2] -
    // E[x]^2 would cancel catastrophically for values near 1 g.
    size_t n = 0;
    uint64_t oldest = t;
    Vec sum;
    float mag_sum = 0;
    for (; n < ring_size_; ++n) {
        const Entry& e = Back(n);
        if (t - e.t >= cfg_.still_window_ms) break;
        sum.x += e.a.x;
        sum.y += e.a.y;
        sum.z += e.a.z;
        mag_sum += e.mag;
        oldest = e.t;
    }
    if (n < 3 || t - oldest + kCoverageSlackMs < cfg_.still_window_ms) return false;
    const float inv = 1.0f / static_cast<float>(n);
    const Vec m{sum.x * inv, sum.y * inv, sum.z * inv};
    Vec var;
    for (size_t i = 0; i < n; ++i) {
        const Entry& e = Back(i);
        var.x += (e.a.x - m.x) * (e.a.x - m.x);
        var.y += (e.a.y - m.y) * (e.a.y - m.y);
        var.z += (e.a.z - m.z) * (e.a.z - m.z);
    }
    const float max_var = std::max({var.x, var.y, var.z}) * inv;
    mean = m;
    return std::sqrt(max_var) < cfg_.still_std_g && std::fabs(mag_sum * inv - 1.0f) < cfg_.still_magnitude_tol_g;
}

float MotionRecognizer::ShakeStd(uint64_t t) const
{
    size_t n = 0;
    uint64_t oldest = t;
    float sum = 0;
    for (; n < ring_size_; ++n) {
        const Entry& e = Back(n);
        if (t - e.t >= cfg_.shake_window_ms) break;
        sum += e.mag;
        oldest = e.t;
    }
    if (n < 3 || t - oldest + kCoverageSlackMs < cfg_.shake_window_ms) return 0;
    const float mean = sum / static_cast<float>(n);
    float var = 0;
    for (size_t i = 0; i < n; ++i) var += (Back(i).mag - mean) * (Back(i).mag - mean);
    return std::sqrt(var / static_cast<float>(n));
}

bool MotionRecognizer::QuietBefore(uint64_t t) const
{
    // A knock is an impulse out of rest; inside ongoing motion it is noise.
    uint64_t oldest = t;
    for (size_t i = 0; i < ring_size_; ++i) {
        const Entry& e = Back(i);
        if (t - e.t > cfg_.knock_quiet_before_ms) break;
        if (Norm(Sub(e.a, rest_)) > cfg_.knock_peak_g * 0.5f) return false;
        oldest = e.t;
    }
    return t - oldest + kCoverageSlackMs >= cfg_.knock_quiet_before_ms;
}

}  // namespace eidolon::pet
