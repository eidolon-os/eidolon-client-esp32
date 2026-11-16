#pragma once

// Body motion judgements from the BMI270 accelerometer.
// Pure logic: fed ~100 Hz samples (g) by the board driver, host-testable.
//
// The IMU sits in the CoreS3, i.e. in the head that the pitch servo tilts.
// Servo motion therefore looks like a change of attitude; SuppressUntil()
// covers it (see there). Tilted/Level compare against the boot pose, so a head
// held at a non-home pitch reads as tilted — the runtime has to weigh that.

#include <array>
#include <cstdint>
#include <vector>

#include "eidolon/pet/pet_sense.h"

namespace eidolon::pet {

// Initial values, not calibrated: every threshold still has to be checked on a
// real StackChan (desk vibration, hand tremor, speaker buzz).
struct MotionConfig {
    // Picked up: |a| off 1 g for long enough AND gravity turned away from rest.
    float pick_deviation_g = 0.25f;
    uint32_t pick_deviation_ms = 120;  // accumulated time above the deviation
    uint32_t pick_window_ms = 1500;    // accumulation resets after this long without deviation
    float pick_angle_deg = 25.0f;
    // Still: per-axis std and |a| error over a short window.
    uint32_t still_window_ms = 300;
    float still_std_g = 0.015f;
    float still_magnitude_tol_g = 0.08f;
    uint32_t put_down_still_ms = 800;
    // Shaken: std of |a|-1 over 1 s, sustained.
    uint32_t shake_window_ms = 1000;
    float shake_std_g = 0.5f;
    uint32_t shake_sustain_ms = 1000;
    uint32_t shake_cooldown_ms = 3000;
    // Knock: short impulse while resting, attitude unchanged afterwards.
    float knock_peak_g = 0.35f;
    uint32_t knock_max_ms = 60;
    uint32_t knock_quiet_before_ms = 100;
    uint32_t knock_settle_ms = 100;
    float knock_attitude_deg = 5.0f;
    // Tilted / Level against the boot pose.
    uint32_t level_learn_ms = 2000;
    float tilt_deg = 12.0f;
    float level_deg = 6.0f;
    uint32_t tilt_hold_ms = 2000;
};

class MotionRecognizer {
public:
    explicit MotionRecognizer(const MotionConfig& config = MotionConfig());

    // Knock events carry the impulse time, so they can be ~100 ms older than
    // the sample that reports them.
    void Update(const ImuSample& sample, std::vector<SenseEvent>& out);

    // Servo motion window. Until t_ms: no Knock (the servo itself knocks), no
    // PickedUp (the head turns and accelerates), Tilted timing restarts, and
    // the rest pose is re-learned afterwards because the head has moved.
    void SuppressUntil(uint64_t t_ms);

    bool held() const { return held_; }
    bool tilted() const { return tilted_; }
    float magnitude() const { return last_magnitude_; }

    struct Vec {
        float x = 0, y = 0, z = 0;
    };
    // Low-passed gravity in the IMU frame (g); zero until the first sample.
    Vec gravity() const { return lp_; }

private:
    struct Entry {
        uint64_t t = 0;
        Vec a;
        float mag = 0;
    };
    static constexpr size_t kRing = 128;  // > 1 s at 100 Hz

    void Push(const Entry& e);
    const Entry& Back(size_t i) const;  // 0 = newest
    bool ComputeStill(uint64_t t, Vec& mean) const;
    float ShakeStd(uint64_t t) const;
    bool QuietBefore(uint64_t t) const;
    void UpdateKnock(const ImuSample& s, const Vec& a, bool suppressed, std::vector<SenseEvent>& out);

    MotionConfig cfg_;

    std::array<Entry, kRing> ring_{};
    size_t ring_head_ = 0;  // next write slot
    size_t ring_size_ = 0;

    bool started_ = false;
    uint64_t last_t_ = 0;
    uint64_t suppress_until_ = 0;
    Vec lp_;  // low-passed gravity estimate (~100 ms)
    float last_magnitude_ = 0;

    bool still_ = false;
    uint64_t still_since_ = 0;
    Vec still_mean_;

    bool rest_valid_ = false;
    Vec rest_;  // mean gravity vector while last resting
    bool held_ = false;
    uint32_t deviation_ms_ = 0;
    uint64_t last_deviation_t_ = 0;

    bool shake_high_ = false;
    bool shake_reported_ = false;
    uint64_t shake_since_ = 0;
    uint64_t last_shake_t_ = 0;
    bool shake_ever_ = false;

    enum class KnockPhase { Idle, Impulse, Settle };
    KnockPhase knock_phase_ = KnockPhase::Idle;
    uint64_t knock_start_ = 0;
    uint64_t knock_last_above_ = 0;
    uint64_t knock_settle_end_ = 0;
    float knock_peak_ = 0;
    Vec knock_sum_;
    uint32_t knock_n_ = 0;

    bool level_valid_ = false;
    Vec level_;
    bool tilted_ = false;
    bool tilt_pending_ = false;
    uint64_t tilt_since_ = 0;
};

}  // namespace eidolon::pet
