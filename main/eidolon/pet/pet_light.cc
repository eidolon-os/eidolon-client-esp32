#include "eidolon/pet/pet_light.h"

#include <algorithm>

namespace eidolon::pet {

namespace {
SenseEvent Event(SenseKind kind, uint64_t t, float value = 0)
{
    SenseEvent e;
    e.kind = kind;
    e.t_ms = t;
    e.value = value;
    return e;
}
}  // namespace

LightRecognizer::LightRecognizer(const LightConfig& config) : cfg_(config) {}

void LightRecognizer::Update(const LightSample& s, std::vector<SenseEvent>& out)
{
    if (s.proximity_valid) UpdateProximity(s.t_ms, s.proximity, out);
    if (s.lux_valid) UpdateLux(s.t_ms, s.lux, out);
    UpdateCovered(s.t_ms, out);
}

void LightRecognizer::IgnoreUntil(uint64_t t_ms)
{
    ignore_until_ = std::max(ignore_until_, t_ms);
    // Readings from before the change are on another scale.
    lux_count_ = 0;
    lux_rebase_ = true;
}

void LightRecognizer::UpdateProximity(uint64_t t, uint16_t p, std::vector<SenseEvent>& out)
{
    const float pf = p;
    if (!prox_init_) {
        prox_base_ = pf;
        prox_init_ = true;
    }
    const float rise = pf - prox_base_;
    const bool at_saturation = p >= cfg_.saturation;

    if (!near_) {
        if (rise > cfg_.near_rise) {
            near_ = was_near_ = true;
            near_since_ = last_near_t_ = t;
            // Arriving straight at saturation is FaceClose/Covered territory;
            // the retreat afterwards must not read as an approach either.
            if (!at_saturation) out.push_back(Event(SenseKind::HandNear, t, rise));
        } else {
            // Crosstalk and temperature move the floor: follow it slowly up,
            // quickly down.
            const float alpha = pf < prox_base_ ? cfg_.prox_baseline_fall_alpha : cfg_.prox_baseline_alpha;
            prox_base_ += alpha * (pf - prox_base_);
        }
    } else {
        last_near_t_ = t;
        if (!at_saturation && rise < cfg_.near_release) {
            near_ = false;
        } else if (!at_saturation && !covered_ && t - near_since_ > cfg_.near_absorb_ms) {
            // Something parked next to the sensor (a cup, a wall) would hold
            // "near" forever and mute the light judgements; make it the floor.
            prox_base_ = pf;
            near_ = false;
        }
    }

    if (at_saturation) {
        if (!saturated_) {
            saturated_ = true;
            saturated_since_ = t;
            face_reported_ = false;
        }
        if (!face_reported_ && !covered_ && t - saturated_since_ >= cfg_.face_close_ms) {
            face_reported_ = true;
            // Light already gone means a cover in progress: Covered follows.
            if (!LuxDropped()) out.push_back(Event(SenseKind::FaceClose, t, pf));
        }
    } else if (p < cfg_.saturation_release) {
        saturated_ = false;
        cover_pending_ = false;
        if (covered_) {
            covered_ = false;
            out.push_back(Event(SenseKind::Uncovered, t, pf));
        }
    }
}

void LightRecognizer::UpdateLux(uint64_t t, float lux, std::vector<SenseEvent>& out)
{
    lux_ = lux;  // Covered needs the current reading even while judgements pause
    if (t < ignore_until_) {
        lux_count_ = 0;
        lux_rebase_ = true;
        return;
    }
    // A hand shades the sensor on its way in and out; neither the history nor
    // the baseline may learn from that, or uncovering reads as lights on.
    if (ProximityRecent(t)) {
        lux_count_ = 0;
        return;
    }
    if (!lux_init_ || lux_rebase_) {
        lux_base_ = lux;
        lux_init_ = true;
        lux_rebase_ = false;
    } else {
        float ref_max = -1, ref_min = 0;
        for (uint8_t i = 0; i < lux_count_; ++i) {
            if (t - lux_hist_[i].t > cfg_.lights_window_ms) continue;
            if (ref_max < 0) ref_max = ref_min = lux_hist_[i].lux;
            ref_max = std::max(ref_max, lux_hist_[i].lux);
            ref_min = std::min(ref_min, lux_hist_[i].lux);
        }
        bool event = false;
        if (ref_max >= cfg_.min_reference_lux && lux < (1.0f - cfg_.lights_off_drop) * ref_max &&
            (!off_ever_ || t - last_off_t_ >= cfg_.lights_cooldown_ms)) {
            out.push_back(Event(SenseKind::LightsOff, t, lux));
            off_ever_ = event = true;
            last_off_t_ = t;
        } else if (ref_max >= 0 && lux > cfg_.lights_on_ratio * ref_min && lux > cfg_.lights_on_min_lux &&
                   (!on_ever_ || t - last_on_t_ >= cfg_.lights_cooldown_ms)) {
            out.push_back(Event(SenseKind::LightsOn, t, lux));
            on_ever_ = event = true;
            last_on_t_ = t;
        }
        if (event) {
            // The room changed: a cover judged against the old level would be
            // satisfied by the dark alone.
            lux_base_ = lux;
            lux_count_ = 0;
        } else {
            lux_base_ += cfg_.lux_baseline_alpha * (lux - lux_base_);
        }
    }
    if (lux_count_ == lux_hist_.size()) {
        std::copy(lux_hist_.begin() + 1, lux_hist_.end(), lux_hist_.begin());
        --lux_count_;
    }
    lux_hist_[lux_count_].t = t;
    lux_hist_[lux_count_].lux = lux;
    ++lux_count_;
}

void LightRecognizer::UpdateCovered(uint64_t t, std::vector<SenseEvent>& out)
{
    if (covered_ || !saturated_ || !LuxDropped()) {
        cover_pending_ = false;
        return;
    }
    if (!cover_pending_) {
        cover_pending_ = true;
        cover_since_ = t;
    } else if (t - cover_since_ > cfg_.covered_ms) {
        cover_pending_ = false;
        covered_ = true;
        out.push_back(Event(SenseKind::Covered, t, lux_));
    }
}

bool LightRecognizer::ProximityRecent(uint64_t t) const
{
    return near_ || saturated_ || (was_near_ && t - last_near_t_ < cfg_.near_grace_ms);
}

bool LightRecognizer::LuxDropped() const
{
    // In a dark room there is no drop to see; Covered cannot be told apart.
    return lux_init_ && lux_base_ >= cfg_.min_reference_lux &&
           lux_ < (1.0f - cfg_.covered_lux_drop) * lux_base_;
}

}  // namespace eidolon::pet
