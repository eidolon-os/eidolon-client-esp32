#pragma once

// Proximity and ambient light judgements from the LTR-553.
// Pure logic: fed proximity ~10 Hz and lux ~2 Hz by the board driver (one
// sample may carry either or both), host-testable.

#include <array>
#include <cstdint>
#include <vector>

#include "eidolon/pet/pet_sense.h"

namespace eidolon::pet {

// Initial values, not calibrated: proximity counts depend on the LED setup and
// the CoreS3 front glass, lux on the window factor. Check on a real unit.
struct LightConfig {
    // Proximity, raw counts above an adaptive baseline.
    float near_rise = 80;               // HandNear when this far above baseline
    float near_release = 40;            // near ends below this
    uint16_t saturation = 1500;         // "almost saturated" (11-bit max 2047)
    uint16_t saturation_release = 1200;
    uint32_t face_close_ms = 300;
    uint32_t covered_ms = 500;
    float covered_lux_drop = 0.7f;      // lux below (1 - drop) x lux baseline
    float prox_baseline_alpha = 0.02f;  // ~5 s at 10 Hz, rising
    float prox_baseline_fall_alpha = 0.2f;
    uint32_t near_absorb_ms = 20000;    // a static object this long becomes baseline
    // Ambient light.
    float lights_off_drop = 0.7f;       // within lights_window_ms
    float lights_on_ratio = 3.0f;
    float lights_on_min_lux = 30.0f;
    uint32_t lights_window_ms = 1000;
    float min_reference_lux = 3.0f;     // below this, ratios are noise
    uint32_t lights_cooldown_ms = 2000;
    uint32_t near_grace_ms = 1000;      // lux ignored this long after a hand was near
    float lux_baseline_alpha = 0.05f;   // ~10 s at 2 Hz
};

class LightRecognizer {
public:
    explicit LightRecognizer(const LightConfig& config = LightConfig());

    void Update(const LightSample& sample, std::vector<SenseEvent>& out);

    // The sensor sits behind the screen glass, so backlight changes move lux.
    // Lux samples before t_ms are ignored and the lux history restarts after.
    void IgnoreUntil(uint64_t t_ms);

    bool near() const { return near_; }
    bool covered() const { return covered_; }
    float proximity_baseline() const { return prox_base_; }
    float lux_baseline() const { return lux_base_; }

private:
    struct LuxPoint {
        uint64_t t = 0;
        float lux = 0;
    };

    void UpdateProximity(uint64_t t, uint16_t p, std::vector<SenseEvent>& out);
    void UpdateLux(uint64_t t, float lux, std::vector<SenseEvent>& out);
    void UpdateCovered(uint64_t t, std::vector<SenseEvent>& out);
    bool ProximityRecent(uint64_t t) const;
    bool LuxDropped() const;

    LightConfig cfg_;

    // Proximity.
    bool prox_init_ = false;
    float prox_base_ = 0;
    bool near_ = false;
    uint64_t near_since_ = 0;
    uint64_t last_near_t_ = 0;
    bool was_near_ = false;
    bool saturated_ = false;
    uint64_t saturated_since_ = 0;
    bool face_reported_ = false;
    bool cover_pending_ = false;
    uint64_t cover_since_ = 0;
    bool covered_ = false;

    // Lux.
    uint64_t ignore_until_ = 0;
    bool lux_init_ = false;
    bool lux_rebase_ = false;
    float lux_ = 0;
    float lux_base_ = 0;
    std::array<LuxPoint, 8> lux_hist_{};  // oldest first
    uint8_t lux_count_ = 0;
    bool off_ever_ = false, on_ever_ = false;
    uint64_t last_off_t_ = 0, last_on_t_ = 0;
};

}  // namespace eidolon::pet
