#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "boards/common/camera.h"

namespace eidolon {

inline constexpr const char* kOwnerFaceModelId =
    "esp-who-human-face-recognition-v1";
inline constexpr const char* kOwnerFacePreprocessingVersion =
    "rgb565-be-qvga-v1";

struct OwnerFaceSyncRequest {
    std::string binding_id;
    std::string profile_id;
    uint32_t profile_revision = 0;
    std::string desired_state;
};

struct OwnerFaceApplyResult {
    OwnerFaceSyncRequest request;
    bool success = false;
    std::string code;
    std::string model_id;
    std::string preprocessing_version;
    uint32_t template_count = 0;
};

struct OwnerFaceLiveResult {
    bool evaluated = false;
    uint64_t evaluated_at_ms = 0;
    uint32_t profile_revision = 0;
    int faces = 0;
    int id = -1;
    float similarity = 0.0f;
};

struct OwnerFaceProfileStatus {
    bool active = false;
    uint32_t revision = 0;
    uint32_t template_count = 0;
};

using OwnerFaceApplyCallback = std::function<void(const OwnerFaceApplyResult&)>;

class OwnerFaceEngine {
public:
    OwnerFaceEngine();
    ~OwnerFaceEngine();

    bool QueueSync(const OwnerFaceSyncRequest& request,
                   const std::string& config_url,
                   const std::string& device_id,
                   OwnerFaceApplyCallback callback);
    OwnerFaceLiveResult AnalyzeLiveFrame(const CameraFrame& frame, uint64_t now_ms);
    void SetLiveIntervalMs(uint32_t interval_ms);
    bool TryGetProfileStatus(OwnerFaceProfileStatus& out);

    OwnerFaceEngine(const OwnerFaceEngine&) = delete;
    OwnerFaceEngine& operator=(const OwnerFaceEngine&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eidolon
