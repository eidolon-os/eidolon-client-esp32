#include "dvp_still_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_video_device.h"
#include "jpg/image_to_jpeg.h"
#include "linux/videodev2.h"
#include "yuyv_scale.h"

namespace {

constexpr char TAG[] = "DvpStill";
constexpr int kBuffers = 2;

uint32_t MsSince(int64_t since_us)
{
    return static_cast<uint32_t>((esp_timer_get_time() - since_us) / 1000);
}

// The open device and everything mapped from it, released in reverse order
// however the capture ends.
struct Device {
    struct Mapping {
        void* start = nullptr;
        size_t length = 0;
    };
    int fd = -1;
    bool streaming = false;
    std::vector<Mapping> mappings;

    void Stop()
    {
        if (!streaming) return;
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) ESP_LOGW(TAG, "stream off failed errno=%d", errno);
        streaming = false;
    }

    ~Device()
    {
        Stop();
        for (const Mapping& mapping : mappings) {
            if (mapping.start != nullptr) munmap(mapping.start, mapping.length);
        }
        if (fd < 0) return;
        // Give the frame buffers back now, not at the next capture.
        v4l2_requestbuffers release = {};
        release.count = 0;
        release.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        release.memory = V4L2_MEMORY_MMAP;
        ioctl(fd, VIDIOC_REQBUFS, &release);
        close(fd);
    }
};

void SetFlip(int fd, uint32_t control, bool enabled)
{
    v4l2_ext_control value = {};
    value.id = control;
    value.value = enabled ? 1 : 0;
    v4l2_ext_controls controls = {};
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = &value;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "flip control 0x%lx not applied", static_cast<unsigned long>(control));
    }
}

// The capture format: YUV422 first (this esp_video's "YUV422P" is packed
// YUYV, the JPEG encoder's native input), RGB565 if that is all there is.
uint32_t PickFormat(int fd)
{
    uint32_t best = 0;
    int best_rank = 100;
    v4l2_fmtdesc description = {};
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (description.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &description) == 0; ++description.index) {
        int rank = 100;
        switch (description.pixelformat) {
        case V4L2_PIX_FMT_YUV422P: rank = 0; break;
        case V4L2_PIX_FMT_YUYV: rank = 1; break;
        case V4L2_PIX_FMT_RGB565: rank = 2; break;
        default: break;
        }
        if (rank < best_rank) {
            best_rank = rank;
            best = description.pixelformat;
        }
    }
    return best;
}

}  // namespace

DvpStillCamera::DvpStillCamera(const esp_video_init_config_t& config, int settle_frames,
                               bool hmirror, bool vflip)
    : settle_frames_(settle_frames < 0 ? 0 : settle_frames), hmirror_(hmirror), vflip_(vflip)
{
    const esp_err_t err = esp_video_init(&config);
    ready_ = err == ESP_OK;
    if (!ready_) ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
}

StillCamera::Error DvpStillCamera::Capture(int max_edge, int quality, Image& out)
{
    if (!ready_) return Error::Unavailable;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    abort_.store(false);
    const int64_t started_us = esp_timer_get_time();

    Device device;
    device.fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (device.fd < 0) {
        ESP_LOGE(TAG, "open failed errno=%d", errno);
        return Error::Unavailable;
    }
    v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(device.fd, VIDIOC_G_FMT, &format) != 0) return Error::Unavailable;
    const uint32_t pixel_format = PickFormat(device.fd);
    if (pixel_format == 0) return Error::Unavailable;
    format.fmt.pix.pixelformat = pixel_format;
    if (ioctl(device.fd, VIDIOC_S_FMT, &format) != 0) return Error::Unavailable;
    int width = static_cast<int>(format.fmt.pix.width);
    int height = static_cast<int>(format.fmt.pix.height);
    const bool yuyv = pixel_format != V4L2_PIX_FMT_RGB565;
    SetFlip(device.fd, V4L2_CID_HFLIP, hmirror_);
    SetFlip(device.fd, V4L2_CID_VFLIP, vflip_);

    v4l2_requestbuffers request = {};
    request.count = kBuffers;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(device.fd, VIDIOC_REQBUFS, &request) != 0 || request.count == 0) return Error::NoMemory;
    device.mappings.resize(request.count);
    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl(device.fd, VIDIOC_QUERYBUF, &buffer) != 0) return Error::NoMemory;
        void* start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device.fd,
                           buffer.m.offset);
        if (start == MAP_FAILED || start == nullptr) return Error::NoMemory;
        device.mappings[index] = {start, buffer.length};
        if (ioctl(device.fd, VIDIOC_QBUF, &buffer) != 0) return Error::NoMemory;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(device.fd, VIDIOC_STREAMON, &type) != 0) return Error::Unavailable;
    device.streaming = true;
    out.open_ms = MsSince(started_us);

    // The first frames after the sensor starts are dark or tinted while its
    // exposure and white balance find the scene; they go back unread.
    const int64_t frames_us = esp_timer_get_time();
    v4l2_buffer kept = {};
    for (int frame = 0; frame <= settle_frames_; ++frame) {
        if (abort_.load()) return Error::Aborted;
        v4l2_buffer buffer = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(device.fd, VIDIOC_DQBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "no frame errno=%d", errno);
            return Error::NoFrame;
        }
        if (frame < settle_frames_) {
            if (ioctl(device.fd, VIDIOC_QBUF, &buffer) != 0) return Error::NoFrame;
            continue;
        }
        kept = buffer;
    }
    out.read_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Nothing more to read: stop the DMA before the encoder needs the bandwidth.
    device.Stop();
    out.frames_ms = MsSince(frames_us);
    out.frames_dropped = static_cast<uint8_t>(settle_frames_);
    if (abort_.load()) return Error::Aborted;

    uint8_t* frame = static_cast<uint8_t*>(device.mappings[kept.index].start);
    size_t frame_size = kept.bytesused;
    if (frame_size < static_cast<size_t>(width) * height * 2) {
        ESP_LOGE(TAG, "short frame %u bytes for %dx%d", static_cast<unsigned>(frame_size), width, height);
        return Error::NoFrame;
    }
    std::unique_ptr<uint8_t, void (*)(void*)> scaled(nullptr, &heap_caps_free);
    const int factor = YuyvScaleFactor(width, height, max_edge);
    if (factor > 1 && yuyv) {
        scaled.reset(static_cast<uint8_t*>(heap_caps_malloc(
            static_cast<size_t>(width / factor) * (height / factor) * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        int scaled_width = 0, scaled_height = 0;
        if (!scaled || !ScaleYuyv(frame, width, height, factor, scaled.get(), scaled_width, scaled_height)) {
            return Error::NoMemory;
        }
        frame = scaled.get();
        width = scaled_width;
        height = scaled_height;
        frame_size = static_cast<size_t>(width) * height * 2;
    } else if (factor > 1) {
        ESP_LOGW(TAG, "max_edge %d below %dx%d not applied to this format", max_edge, width, height);
    }

    const int64_t encode_us = esp_timer_get_time();
    uint8_t* jpeg = nullptr;
    size_t jpeg_size = 0;
    const bool encoded = image_to_jpeg(frame, frame_size, static_cast<uint16_t>(width),
                                       static_cast<uint16_t>(height),
                                       yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_RGB565,
                                       static_cast<uint8_t>(quality), &jpeg, &jpeg_size);
    out.encode_ms = MsSince(encode_us);
    if (!encoded || jpeg == nullptr || jpeg_size == 0) {
        free(jpeg);
        return Error::NoMemory;
    }
    out.data.reset(jpeg);
    out.size = jpeg_size;
    out.width = width;
    out.height = height;
    return Error::None;
}
