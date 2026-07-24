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
    // x,y in [-1,1]. ttl_ms > 0 arms a guardrail: the head returns home when the hold
    // expires, so no caller can leave it turned off-center indefinitely.
    void LookAtNormalized(float x, float y, int speed = 500, int ttl_ms = 0);
    void GoHome(int speed = 500);
    // Emergency stop (safety.stop): preempt any running gesture and cut servo torque so
    // the head goes limp. Highest motion priority. The next command re-engages torque.
    void Stop();

    // Mic-capture quiet gate. quiet=true cuts the servo power rail (PY32 VM EN) so the
    // servo DC-DC/PWM switching whine can't couple into the on-board mic during uplink
    // capture (head goes limp); quiet=false restores the rail so motion can resume.
    // Idle torque-release alone does NOT silence it — the rail keeps switching — so the
    // rail itself must be powered down. While quiet, motion commands are dropped and the
    // 50 Hz animation tick is skipped (the servos are unpowered).
    void SetCaptureQuiet(bool quiet);

    // Discrete expressive gesture: name in {nod, shake, perk_up, droop, glance}.
    // Runs a short motion sequence in a one-shot task; a new gesture is dropped while
    // one is running. Unused params per gesture are ignored.
    void HeadGesture(const std::string& name, int times, float x, float y,
                     int hold_ms, int return_ms);

    // Boot bring-up sweep in its own task (does not block board construction).
    void StartSelfTest();

    // 12-LED RGB ring on the PY32 IO-expander. RgbMarquee runs a one-shot cyan
    // chase (cute wake effect); RgbOff clears the ring. No-ops if the IO-expander
    // is absent. A running marquee is preempted by RgbOff / a new marquee.
    void RgbMarquee();
    void RgbOff();

private:
    void SelfTest();
    void RunGesture();
    void RunRgbMarquee();
    void SetAllLeds(uint8_t r, uint8_t g, uint8_t b);
    void UpdateLoop();
    // Delay one gesture step, then report whether the gesture may continue (false if a
    // safety stop was requested or the body went unready). Keeps a running gesture from
    // fighting a safety.stop.
    bool GestureContinue(int delay_ms);

    i2c_master_bus_handle_t i2c_bus_;
    m5::PY32IOExpander_Class* ioe_ = nullptr;
    SCSCL scs_;
    std::unique_ptr<stackchan::motion::Motion> motion_;
    SemaphoreHandle_t motion_mutex_ = nullptr;
    bool ready_ = false;
    // True while the mic is hot for uplink: the servo power rail is cut and motion is
    // suppressed so servo switching whine can't corrupt the captured audio. Read on the
    // 50 Hz update tick and by the motion command guards; written under motion_mutex_.
    volatile bool capture_quiet_ = false;

    // Pending gesture params, consumed by the one-shot gesture task. Guarded by
    // gesture_busy_ (only one gesture runs at a time).
    volatile bool gesture_busy_ = false;
    // Set by Stop() to preempt the running gesture; cleared when a new gesture starts.
    volatile bool gesture_abort_ = false;
    // RGB marquee one-shot task state (LED ring lives on the PY32, separate from the
    // servo bus). rgb_abort_ preempts a running marquee (RgbOff / new marquee).
    volatile bool rgb_busy_ = false;
    volatile bool rgb_abort_ = false;
    // Absolute esp_timer deadline (us) for a look_at TTL hold; 0 = disarmed. Enforced on
    // the 50 Hz update tick so the guardrail can't be bypassed by any caller.
    volatile int64_t look_at_deadline_us_ = 0;
    struct {
        std::string name;
        int times;
        float x, y;
        int hold_ms, return_ms;
    } gesture_{};
};
