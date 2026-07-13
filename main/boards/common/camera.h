#ifndef CAMERA_H
#define CAMERA_H

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_EIDOLON_GUARD_SERVICE
#define CONFIG_EIDOLON_GUARD_SERVICE 0
#endif

#if CONFIG_EIDOLON_GUARD_SERVICE
#include <cstddef>
#include <cstdint>
#include <functional>
#endif
#include <string>

#if CONFIG_EIDOLON_GUARD_SERVICE
// The buffer belongs to the camera implementation and is only valid while the
// analyzer callback runs. Consumers must not retain the pointer.
struct CameraFrame {
    const uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t pixel_format = 0;
};

using CameraFrameAnalyzer = std::function<bool(const CameraFrame& frame)>;
#endif

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;
#if CONFIG_EIDOLON_GUARD_SERVICE
    virtual bool AnalyzeFrame(const CameraFrameAnalyzer& analyzer) {
        (void)analyzer;
        return false;
    }
#endif
};

#endif // CAMERA_H
