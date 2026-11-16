#pragma once

// StackChan body sensors on the shared CoreS3 I2C bus (I2C1, SDA 12 / SCL 11):
//   Si12T    0x68  head-top three-zone touch pad (StackChan body)
//   LTR-553  0x23  proximity + ambient light (CoreS3, above the screen)
//   BMI270   0x69  accelerometer (CoreS3, i.e. in the moving head)
// One low-priority task polls all three and turns samples into SenseEvents
// through the pure recognizers in eidolon/pet. Every device is optional.

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "eidolon/pet/pet_light.h"
#include "eidolon/pet/pet_motion.h"
#include "eidolon/pet/pet_sense.h"
#include "eidolon/pet/pet_touch.h"

class StackChanSensors {
public:
    using Sink = std::function<void(const eidolon::pet::SenseEvent&)>;

    StackChanSensors();
    // Create once and keep: the polling task is never stopped.
    StackChanSensors(const StackChanSensors&) = delete;
    StackChanSensors& operator=(const StackChanSensors&) = delete;

    // Adds whichever devices answer to the existing bus and configures them.
    // Blocks ~0.3 s when the BMI270 is present (8 KB feature blob upload).
    // Returns true if at least one device came up.
    bool Init(i2c_master_bus_handle_t bus);

    // Starts the single polling task (core 1, priority 10, 4 KB stack). The
    // sink is called from that task: keep it to an enqueue.
    void Start(Sink sink);

    // Times are esp_timer milliseconds (esp_timer_get_time() / 1000). Safe
    // from any task; applied at the start of the next 25 ms poll, so call it
    // when the motion is commanded, not after it started.
    // The head (and the IMU in it) is moving under its own servos until until_ms.
    // Applies to the IMU only; the touch pad keeps reporting.
    void SuppressMotionArtifacts(uint64_t until_ms);
    void IgnoreLightUntil(uint64_t until_ms);

    bool has_touch() const { return touch_.dev != nullptr; }
    bool has_imu() const { return imu_.dev != nullptr; }
    bool has_light() const { return light_.dev != nullptr; }
    // Low-passed gravity in the head's IMU frame, milli-g. Diagnostics only:
    // read from another task without a lock (a torn read is harmless here).
    bool GravityMg(int& x, int& y, int& z) const;

private:
    struct Device {
        const char* name = "";
        uint8_t addr = 0;
        uint32_t scl_hz = 0;
        bool (StackChanSensors::*configure)() = nullptr;
        i2c_master_dev_handle_t dev = nullptr;
        uint8_t failures = 0;     // consecutive
        uint32_t errors = 0;      // total, for the diagnostics line
        uint32_t backoff_ms = 0;
        uint64_t next_try_ms = 0;
        bool needs_reinit = false;
    };

    static constexpr size_t kFifoBufferBytes = 300;  // 0.5 s of 6-byte frames at 100 Hz

    bool AddDevice(Device& d);
    void RemoveDevice(Device& d);
    bool Write(Device& d, const uint8_t* data, size_t len);
    bool WriteReg(Device& d, uint8_t reg, uint8_t value);
    bool ReadRegs(Device& d, uint8_t reg, uint8_t* out, size_t len);

    bool ConfigureSi12t();
    bool ResetSi12tReference();
    bool ConfigureLtr553();
    bool ConfigureBmi270();

    void Run();
    void ApplyWindows();
    template <typename ReadFn>
    void Poll(Device& d, uint64_t now, ReadFn&& read);
    void Succeed(Device& d);
    void Fail(Device& d, uint64_t now);
    bool PollTouch(uint64_t now);
    bool PollImu(uint64_t now);
    bool PollLight(uint64_t now, bool proximity, bool lux);
    void LogDiagnostics();

    i2c_master_bus_handle_t bus_ = nullptr;
    Device touch_, light_, imu_;
    TaskHandle_t task_ = nullptr;
    Sink sink_;
    std::vector<eidolon::pet::SenseEvent> events_;

    eidolon::pet::HeadTouchRecognizer touch_rec_;
    eidolon::pet::MotionRecognizer motion_rec_;
    eidolon::pet::LightRecognizer light_rec_;

    // Windows requested by other tasks; the poll task forwards them.
    portMUX_TYPE lock_;
    uint64_t pending_suppress_ = 0, pending_ignore_ = 0;
    uint64_t applied_suppress_ = 0, applied_ignore_ = 0;

    uint8_t fifo_[kFifoBufferBytes] = {};
    uint64_t last_imu_t_ = 0;
    uint32_t imu_overflows_ = 0;
    uint64_t last_recal_ms_ = 0;
    uint32_t recalibrations_ = 0;
    uint32_t logged_presses_ = 0;   // touch presses already written to the log

    // Last readings, for the periodic diagnostics line.
    uint8_t last_touch_ = 0;
    uint16_t last_prox_ = 0;
    float last_lux_ = 0;
};
