#include "radar_presence_tracker.h"

#include <algorithm>

RadarPresenceTracker::RadarPresenceTracker(RadarPresenceConfig config)
    : config_(config)
{
    config_.warmup_ms = std::max<uint32_t>(config_.warmup_ms, 100);
    config_.enter_debounce_ms = std::max<uint32_t>(config_.enter_debounce_ms, 50);
    config_.absence_hold_ms = std::max<uint32_t>(config_.absence_hold_ms, 1000);
}

void RadarPresenceTracker::SetAvailable(bool available, uint64_t now_ms)
{
    if (available_ == available) {
        return;
    }
    available_ = available;
    tracking_high_ = false;
    high_since_ms_ = 0;
    last_detected_ms_ = 0;
    if (available) {
        available_since_ms_ = now_ms;
        state_ = RadarPresenceState::Calibrating;
    } else {
        available_since_ms_ = 0;
        state_ = RadarPresenceState::Unavailable;
    }
}

RadarPresenceState RadarPresenceTracker::Update(bool raw_detected, uint64_t now_ms)
{
    if (!available_) {
        state_ = RadarPresenceState::Unavailable;
        return state_;
    }

    if (raw_detected) {
        if (!tracking_high_) {
            tracking_high_ = true;
            high_since_ms_ = now_ms;
        }
    } else {
        tracking_high_ = false;
        high_since_ms_ = 0;
    }

    if (state_ == RadarPresenceState::Calibrating) {
        if (now_ms - available_since_ms_ < config_.warmup_ms) {
            return state_;
        }
        if (raw_detected &&
            now_ms - high_since_ms_ >= config_.enter_debounce_ms) {
            state_ = RadarPresenceState::Present;
            last_detected_ms_ = now_ms;
        } else {
            state_ = RadarPresenceState::Vacant;
        }
        return state_;
    }

    if (state_ == RadarPresenceState::Vacant) {
        if (raw_detected &&
            now_ms - high_since_ms_ >= config_.enter_debounce_ms) {
            state_ = RadarPresenceState::Present;
            last_detected_ms_ = now_ms;
        }
        return state_;
    }

    if (raw_detected) {
        last_detected_ms_ = now_ms;
    } else if (last_detected_ms_ != 0 &&
               now_ms - last_detected_ms_ >= config_.absence_hold_ms) {
        state_ = RadarPresenceState::Vacant;
    }
    return state_;
}

const char* RadarPresenceStateName(RadarPresenceState state)
{
    switch (state) {
    case RadarPresenceState::Unavailable:
        return "unavailable";
    case RadarPresenceState::Calibrating:
        return "calibrating";
    case RadarPresenceState::Vacant:
        return "vacant";
    case RadarPresenceState::Present:
        return "present";
    }
    return "unknown";
}
