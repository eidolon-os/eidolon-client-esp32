#pragma once

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <memory>
#include <atomic>
#include <functional>
#include "boards/common/head_motion.h"
#include "eidolon/pet/pet_face.h"
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
                     int hold_ms, int return_ms, eidolon::HeadMotionObserver observer = {});

    // Boot bring-up sweep in its own task (does not block board construction).
    void StartSelfTest();

    // 12-LED RGB ring on the PY32 IO-expander. RgbMarquee runs a one-shot cyan
    // chase (cute wake effect); RgbOff clears the ring. No-ops if the IO-expander
    // is absent. A running marquee is preempted by RgbOff / a new marquee.
    void RgbMarquee();
    void RgbOff();

    // --- Desk pet (CONFIG_EIDOLON_PET_FACE) -------------------------------------------
    // The pet runtime owns the ring: frames are written from the update task at most
    // every 60 ms and only when they change (one bulk write). While pet LEDs are on,
    // RgbMarquee / RgbOff from the legacy presence reflex are ignored.
    // rgb: 12 triples, left column top->bottom then right column top->bottom.
    void SetPetLedFrame(const uint8_t rgb[36]);
    // Local head clip (pet reflex / attention / idle glance). Lower priority than every
    // server command: refused while a server gesture runs, a look_at hold is armed, a
    // server move happened in the last 2 s, capture holds the head, or a clip of higher
    // priority (smaller number) is playing. Any server command cancels a running clip.
    bool PlayLocalClip(const eidolon::pet::HeadClip& clip);
    // Cancels a local clip (or its pending return) and drives home now. Server gestures
    // and a hot microphone are left alone.
    void SettleLocalClip();
    // Called from the update task when someone turns the head by hand (pushed=true,
    // dir = sign of the yaw offset) and when they let go (pushed=false).
    void SetPushCallback(std::function<void(bool pushed, int dir)> callback);
    // esp_timer time (us) of the last commanded or physical head motion; IMU and touch
    // recognizers ignore their own body's movement until a short time after this.
    int64_t last_motion_us() const { return last_motion_us_.load(); }
    // Told (under the motion lock, keep it to a store) whenever the head is about to
    // move or is moving, with the time until which its IMU readings are suspect.
    void SetMotionObserver(std::function<void(int64_t until_us)> observer);
    // True once the head has settled at its home pitch. The IMU sits in the head, so a
    // "tilted" reading only means the desk is tilted when this holds.
    bool head_level() const { return head_level_.load(); }

private:
    eidolon::HeadMotionObserver gesture_observer_;
    std::atomic<int64_t> power_ready_us_{0};
    void SelfTest();
    void RunGesture();
    void RunRgbMarquee();
    void SetAllLeds(uint8_t r, uint8_t g, uint8_t b);
    void UpdateLoop();
    // Marks a server-originated motion: cancels a local clip. Call with motion_mutex_ held.
    void ServerMotionLocked();
    void StepLocalClipLocked(int64_t now_us);
    void SensePushLocked(int64_t now_us);
    void FlushPetLeds();
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
    std::atomic<bool> capture_quiet_ = false;

    // Pending gesture params, consumed by the one-shot gesture task. Guarded by
    // gesture_busy_ (only one gesture runs at a time).
    std::atomic<bool> gesture_busy_ = false;
    // Set by Stop() to preempt the running gesture; cleared when a new gesture starts.
    std::atomic<bool> gesture_abort_ = false;
    // RGB marquee one-shot task state (LED ring lives on the PY32, separate from the
    // servo bus). rgb_abort_ preempts a running marquee (RgbOff / new marquee).
    volatile bool rgb_busy_ = false;
    volatile bool rgb_abort_ = false;
    // Absolute esp_timer deadline (us) for a look_at TTL hold; 0 = disarmed. Enforced on
    // the 50 Hz update tick so the guardrail can't be bypassed by any caller.
    volatile int64_t look_at_deadline_us_ = 0;
    // Local clip state (guarded by motion_mutex_).
    bool local_clip_active_ = false;
    eidolon::pet::HeadClip local_clip_{};
    size_t local_clip_next_ = 0;
    int64_t local_clip_start_us_ = 0;
    int64_t local_home_due_us_ = 0;       // return home after a clip left the head off-centre
    int64_t last_server_motion_us_ = 0;
    std::atomic<int64_t> last_motion_us_{0};
    std::atomic<bool> head_level_{false};
    std::function<void(int64_t)> motion_observer_;
    void NoteMotionLocked(int64_t now_us, int64_t suspect_us);
    // Push detection (update task only).
    std::function<void(bool, int)> push_cb_;
    bool rest_baseline_valid_ = false;
    int rest_yaw_ = 0, rest_pitch_ = 0;
    int64_t rest_since_us_ = 0, next_push_sample_us_ = 0, push_stable_since_us_ = 0;
    int push_hits_ = 0, push_last_yaw_ = 0;
    bool pushed_ = false;
    // Pet LED frame: written by the display task, flushed by the update task.
    portMUX_TYPE led_mux_ = portMUX_INITIALIZER_UNLOCKED;
    std::array<uint8_t, 36> led_pending_{};
    std::array<uint8_t, 36> led_written_{};
    bool led_dirty_ = false;
    bool led_written_valid_ = false;
    std::atomic<bool> pet_leds_ = false;
    int64_t next_led_flush_us_ = 0;
    struct {
        std::string name;
        int times;
        float x, y;
        int hold_ms, return_ms;
    } gesture_{};
};
