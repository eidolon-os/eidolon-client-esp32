#pragma once

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <string>

#include "SCSCL.h"

namespace m5 {
class PY32IOExpander_Class;
}
namespace stackchan {
namespace motion {
class Motion;
}
}  // namespace stackchan

// StackChan head body: two Feetech SCSCL servos (yaw/pitch) powered through the PY32
// IO-expander, driven by the factory StackChan motion stack ported into eidolon
// (stackchan::motion::Motion + ScsServo): spring-eased motion, per-unit NVS zero
// calibration, and software stall protection. A 50 Hz task ticks the spring animation.
//
// The public API is in whole degrees (converted to the motion layer's tenths-of-a-degree
// internally). All command entry points and the update tick are serialized by a mutex,
// since motion commands and the animation tick both touch the servo bus.
class StackChanBody {
public:
    explicit StackChanBody(i2c_master_bus_handle_t i2c_bus);
    ~StackChanBody();

    // Enables the servo power rail (PY32 VM EN), brings up the SCSCL bus, builds the
    // yaw/pitch servos + Motion, starts the update task and homes. Returns false if the
    // IO-expander or servo bus is unavailable (body stays disabled; screen avatar works).
    bool Init();
    bool ready() const { return ready_; }

    void SetHeadAngles(float yaw_deg, float pitch_deg, int speed = 500);
    void LookAtNormalized(float x, float y, int speed = 500);  // x,y in [-1,1]
    void GoHome(int speed = 500);
    void Stop();

    // Discrete expressive gesture: name in {nod, shake, perk_up, droop, glance}.
    // Runs a short motion sequence in a one-shot task; a new gesture is dropped while
    // one is running. Unused params per gesture are ignored.
    void HeadGesture(const std::string& name, int times, float x, float y,
                     int hold_ms, int return_ms);

    // Boot bring-up sweep in its own task (does not block board construction).
    void StartSelfTest();

private:
    void SelfTest();
    void RunGesture();
    void UpdateLoop();

    i2c_master_bus_handle_t i2c_bus_;
    m5::PY32IOExpander_Class* ioe_ = nullptr;
    SCSCL scs_;
    std::unique_ptr<stackchan::motion::Motion> motion_;
    SemaphoreHandle_t motion_mutex_ = nullptr;
    bool ready_ = false;

    // Pending gesture params, consumed by the one-shot gesture task. Guarded by
    // gesture_busy_ (only one gesture runs at a time).
    volatile bool gesture_busy_ = false;
    struct {
        std::string name;
        int times;
        float x, y;
        int hold_ms, return_ms;
    } gesture_{};
};
