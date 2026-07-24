#pragma once

#include <cstdint>

enum class RadarPresenceState {
    Unavailable,
    Calibrating,
    Vacant,
    Present,
};

struct RadarPresenceConfig {
    uint32_t warmup_ms = 2000;
    uint32_t enter_debounce_ms = 200;
    uint32_t absence_hold_ms = 5000;
};

class RadarPresenceTracker {
public:
    explicit RadarPresenceTracker(RadarPresenceConfig config = {});

    void SetAvailable(bool available, uint64_t now_ms);
    RadarPresenceState Update(bool raw_detected, uint64_t now_ms);

    RadarPresenceState state() const { return state_; }
    bool available() const { return available_; }

private:
    RadarPresenceConfig config_;
    RadarPresenceState state_ = RadarPresenceState::Unavailable;
    bool available_ = false;
    bool tracking_high_ = false;
    uint64_t available_since_ms_ = 0;
    uint64_t high_since_ms_ = 0;
    uint64_t last_detected_ms_ = 0;
};

const char* RadarPresenceStateName(RadarPresenceState state);
