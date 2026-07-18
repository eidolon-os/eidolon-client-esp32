#pragma once

#include <cstdint>
#include <memory>

#include "boards/common/camera.h"

namespace eidolon {

struct PersonPresenceResult {
    bool evaluated = false;
    bool present = false;
    float person_score = 0.0f;
    float no_person_score = 0.0f;
    uint64_t evaluated_at_ms = 0;
    uint64_t inference_us = 0;
};

// ATK-only local environmental evidence. This model can renew an identity
// session established by OwnerFaceEngine, but can never establish identity.
class PersonPresenceEngine {
public:
    PersonPresenceEngine();
    ~PersonPresenceEngine();

    PersonPresenceResult AnalyzeFrame(const CameraFrame& frame, uint64_t now_ms);
    void SetIntervalMs(uint32_t interval_ms);
    bool ready() const;

    PersonPresenceEngine(const PersonPresenceEngine&) = delete;
    PersonPresenceEngine& operator=(const PersonPresenceEngine&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eidolon
