#pragma once

#include <string>

class Camera;

// EID-12 probe: capture low-rate camera frames without retaining image data,
// calculate a small luminance grid, and report motion/resource measurements.
class GuardVisionBenchmark {
public:
    static std::string Run(Camera& camera, int sample_count, int interval_ms);
};
