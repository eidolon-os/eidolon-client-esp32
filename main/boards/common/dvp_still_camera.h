#ifndef DVP_STILL_CAMERA_H
#define DVP_STILL_CAMERA_H

#include <atomic>
#include <mutex>

#include "esp_video_init.h"
#include "still_camera.h"

// A DVP sensor (esp_video V4L2) that streams only while a still is taken.
//
// The sensor and its video device are set up once, at boot. A capture opens
// the device, maps two frame buffers (PSRAM), starts the stream, drops the
// first frames while exposure and white balance settle, keeps the next one,
// stops the stream and encodes that frame to JPEG. The DVP controller and its
// DMA exist only between start and stop, which is what keeps a camera board's
// conversation memory where it is without a camera.
class DvpStillCamera : public StillCamera {
public:
    // `config` is handed to esp_video_init here and not kept. `settle_frames`
    // are dropped before the kept one.
    DvpStillCamera(const esp_video_init_config_t& config, int settle_frames,
                   bool hmirror = false, bool vflip = false);

    bool ready() const { return ready_; }
    Error Capture(int max_edge, int quality, Image& out) override;
    void Abort() override { abort_.store(true); }

private:
    bool ready_ = false;
    const int settle_frames_;
    const bool hmirror_;
    const bool vflip_;
    std::atomic<bool> abort_{false};
    std::mutex capture_mutex_;  // one still at a time, whatever asks
};

#endif  // DVP_STILL_CAMERA_H
