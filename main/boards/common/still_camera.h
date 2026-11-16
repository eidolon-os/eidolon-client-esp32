#ifndef STILL_CAMERA_H
#define STILL_CAMERA_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

// One still on request (camera.capture v1). The camera is opened for the
// still and released after it, so nothing streams while nobody is looking:
// no sensor DMA competing with the display and audio for PSRAM, and no camera
// buffers held through a conversation that never asks to look.
//
// Unlike Camera (the xiaozhi photo/explain path), a still never leaves the
// device on its own: the caller decides whom it is sent to.
class StillCamera {
public:
    enum class Error { None, Unavailable, NoFrame, NoMemory, Aborted };

    struct Image {
        // JPEG bytes, heap-allocated (PSRAM); released with free().
        std::unique_ptr<uint8_t, void (*)(void*)> data{nullptr, &std::free};
        size_t size = 0;
        int width = 0;
        int height = 0;
        int64_t read_epoch_ms = 0;  // wall clock when the frame was read
        // Where the time went, for the field log and the camera gate.
        uint32_t open_ms = 0;       // device opened, buffers mapped, stream started
        uint32_t frames_ms = 0;     // first frame through the one kept
        uint32_t encode_ms = 0;
        uint8_t frames_dropped = 0; // while exposure settled
    };

    virtual ~StillCamera() = default;
    // Blocking: call from a worker task, never the UI, audio or controller task.
    // `max_edge` bounds the longer side of the result; `quality` is JPEG 1..100.
    virtual Error Capture(int max_edge, int quality, Image& out) = 0;
    // Ask a capture in progress to give up early. Safe from any task.
    virtual void Abort() = 0;
};

#endif  // STILL_CAMERA_H
