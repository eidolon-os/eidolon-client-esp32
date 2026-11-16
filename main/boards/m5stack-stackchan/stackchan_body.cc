#include "stackchan_body.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "PY32IOExpander_Class.hpp"
#include "motion.h"    // stackchan::motion::Motion
#include "scs_servo.h"  // stackchan::motion::ScsServo + ServoConfig_t

#define TAG "StackChanBody"

using stackchan::motion::Motion;
using stackchan::motion::ScsServo;
using stackchan::motion::ServoConfig_t;

namespace {
// Rest pose (degrees). Yaw centred; pitch near the bottom of the reachable range
// (see config.h for the measurement). Gestures and pet clips are placed relative to
// it and inside [SERVO_PITCH_MIN_DEG, SERVO_PITCH_MAX_DEG].
constexpr float kHomeYawDeg = 0.0f;
constexpr float kHomePitchDeg = SERVO_PITCH_REST_DEG;
constexpr float kPitchFloorDeg = SERVO_PITCH_MIN_DEG;
constexpr float kPitchTopDeg = SERVO_PITCH_MAX_DEG;
// Pitch for a lift relative to rest (+ up), kept inside the reachable range.
float RestPitch(float lift_deg) {
    return std::clamp(kHomePitchDeg + lift_deg, kPitchFloorDeg, kPitchTopDeg);
}
constexpr int kDefaultSpeed = 500;
constexpr int kLedCount = 12;  // RGB ring on the PY32 IO-expander
// 2026-11-15 on-device measurement (docs/stackchan-desk-pet-design.md §6.6): the
// microphone noise floor did not track the servo rail (rail off −63…−40 dBFS, rail
// on −62…−48 dBFS, an intermittent drifting 1.8–2.8 kHz tone either way), while a
// moving head is loud (−25 dBFS) and a re-powered servo needs ~750 ms to answer.
// So capture holds the head still on a powered rail; the rail cut stays available.
#if CONFIG_EIDOLON_STACKCHAN_CAPTURE_CUTS_SERVO_RAIL
constexpr bool kCaptureCutsRail = true;
#else
constexpr bool kCaptureCutsRail = false;
#endif

// Pet ring mapping: runtime order is left column top->bottom, then right column
// top->bottom; value = physical PY32 LED index. Factory order is assumed to be
// 0..5 on one side and 6..11 on the other; adjust here if the device disagrees.
constexpr uint8_t kPetLedMap[kLedCount] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr int64_t kLedFlushPeriodUs = 80 * 1000;       // <= 12.5 Hz, only on change
// IMU and light readings stay suspect this long after a local path stops moving.
constexpr int64_t kLocalSuspectTailUs = 900 * 1000;
constexpr int kPushThresholdDd = 90;                   // 9 degrees off the rest pose
constexpr int64_t kPushSamplePeriodUs = 200 * 1000;
constexpr int64_t kRestSettleUs = 400 * 1000;
constexpr int64_t kPushReleaseStableUs = 1200 * 1000;

// The range a local path stays inside, relative to the rest pose (deci-degrees).
const eidolon::pet::HeadPathLimits kLocalLimits = [] {
    eidolon::pet::HeadPathLimits l;
    l.yaw_lo = SERVO_YAW_MIN_DEG * 10.0f;
    l.yaw_hi = SERVO_YAW_MAX_DEG * 10.0f;
    l.pitch_lo = (kPitchFloorDeg - kHomePitchDeg) * 10.0f;
    l.pitch_hi = (kPitchTopDeg - kHomePitchDeg) * 10.0f;
    return l;
}();

class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t m) : m_(m) {
        if (m_) xSemaphoreTake(m_, portMAX_DELAY);
    }
    ~MutexGuard() {
        if (m_) xSemaphoreGive(m_);
    }
private:
    SemaphoreHandle_t m_;
};
}  // namespace

StackChanBody::StackChanBody(i2c_master_bus_handle_t i2c_bus) : i2c_bus_(i2c_bus), local_(kLocalLimits) {
    motion_mutex_ = xSemaphoreCreateMutex();
}

StackChanBody::~StackChanBody() = default;

bool StackChanBody::Init() {
    // 1) Bring up the PY32 IO-expander (boots slowly; retry up to 1.2 s).
    ioe_ = new m5::PY32IOExpander_Class(i2c_bus_, PY32_IOE_I2C_ADDR);
    const int64_t start_us = esp_timer_get_time();
    bool ioe_ok = false;
    while ((esp_timer_get_time() - start_us) < 1200 * 1000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (ioe_->begin()) {
            ioe_ok = true;
            break;
        }
        ESP_LOGW(TAG, "PY32 IO-expander init failed, retrying...");
    }
    if (!ioe_ok) {
        ESP_LOGE(TAG, "PY32 IO-expander not found — servo body disabled");
        delete ioe_;
        ioe_ = nullptr;
        return false;
    }

    // 2) Enable the servo power rail (VM EN on PY32 pin 0), then let it settle.
    ioe_->setDirection(PY32_SERVO_POWER_PIN, true);
    ioe_->setPullMode(PY32_SERVO_POWER_PIN, true);
    ioe_->digitalWrite(PY32_SERVO_POWER_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Bring up the 12-LED RGB ring (WS2812-style data on PY32 pin 13). The data pin
    // MUST be configured as push-pull output first or the LEDs never light — mirror
    // the factory sequence (config pin -> count -> settle -> clear twice).
    ioe_->setDirection(PY32_RGB_DATA_PIN, true);    // output
    ioe_->setPullMode(PY32_RGB_DATA_PIN, true);     // pull-up
    ioe_->setDriveMode(PY32_RGB_DATA_PIN, false);   // push-pull
    ioe_->setLedCount(kLedCount);
    vTaskDelay(pdMS_TO_TICKS(200));
    SetAllLeds(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    SetAllLeds(0, 0, 0);

    // 3) Bring up the SCSCL servo bus.
    if (!scs_.begin(SERVO_UART_PORT, SERVO_UART_BAUD, SERVO_UART_TX_PIN, SERVO_UART_RX_PIN)) {
        ESP_LOGE(TAG, "SCSCL servo bus begin() failed — servo body disabled");
        return false;
    }

    // 4) Build yaw/pitch servos (angle limits in tenths of a degree) + Motion.
    ServoConfig_t yaw_cfg;
    yaw_cfg.id = SERVO_ID_YAW;
    yaw_cfg.defaultZeroPos = SERVO_YAW_HOME_RAW;
    yaw_cfg.angleLimit = uitk::Vector2i(static_cast<int>(SERVO_YAW_MIN_DEG * 10),
                                        static_cast<int>(SERVO_YAW_MAX_DEG * 10));
    yaw_cfg.rawPosLimit = uitk::Vector2i(SERVO_RAW_MIN, SERVO_RAW_MAX);
    yaw_cfg.settingNs = "servo";
    yaw_cfg.settingZeroPositionKey = "zero_pos_1";

    ServoConfig_t pitch_cfg;
    pitch_cfg.id = SERVO_ID_PITCH;
    pitch_cfg.defaultZeroPos = SERVO_PITCH_HOME_RAW;
    pitch_cfg.angleLimit = uitk::Vector2i(static_cast<int>(SERVO_PITCH_MIN_DEG * 10),
                                          static_cast<int>(SERVO_PITCH_MAX_DEG * 10));
    pitch_cfg.rawPosLimit = uitk::Vector2i(SERVO_RAW_MIN, SERVO_RAW_MAX);
    pitch_cfg.settingNs = "servo";
    pitch_cfg.settingZeroPositionKey = "zero_pos_2";
    pitch_cfg.enableStallProtection = true;  // pitch avoids its mechanical extreme

    auto yaw_servo = std::make_unique<ScsServo>(yaw_cfg, scs_);
    auto pitch_servo = std::make_unique<ScsServo>(pitch_cfg, scs_);
    motion_ = std::make_unique<Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion_->init();
    ready_ = true;

    // 5) Drive the spring animation at 50 Hz on the app CPU.
    xTaskCreatePinnedToCore(
        [](void* arg) { static_cast<StackChanBody*>(arg)->UpdateLoop(); },
        // Priority 9: this loop holds the shared I2C bus while it flushes the LED ring;
        // starved mid-transfer by the network stack (a DTLS handshake keeps core 1
        // busy for 100+ ms) it would make every other bus user time out.
        "stackchan_motion", 4096, this, 9, nullptr, 1);

    ESP_LOGI(TAG, "servo body ready");
    GoHome();
    return true;
}

void StackChanBody::UpdateLoop() {
    for (;;) {
        {
            MutexGuard lock(motion_mutex_);
            // While the mic is hot the servo rail is powered down for silent capture:
            // the servos are dead, so skip the animation tick (driving the SCSCL bus
            // now would write to unpowered servos and can stall the UART). capture_quiet_
            // is read under the mutex so it stays consistent with the rail state set in
            // SetCaptureQuiet().
            // Legacy rail-cut policy: the servos are unpowered while the mic is hot, so the
            // bus must stay quiet. Hold policy: the rail stays up and only motion pauses.
            const int64_t now_us = esp_timer_get_time();
            if (motion_ && !capture_quiet_) {
                StepLocalClipLocked(now_us);
                SensePushLocked(now_us);
            }
            if (motion_ && (!capture_quiet_ || !kCaptureCutsRail)) motion_->update();
        }
        FlushPetLeds();
        // Guardrail: a look_at with ttl_ms returns home when its hold expires. Enforced
        // here (not in the caller) so no motion source can hold the head off-center past
        // its TTL. GoHome takes the mutex itself, so call it outside the block above.
        // Skipped while quiet (no power / no motion to unwind).
        if (!capture_quiet_) {
            int64_t deadline = look_at_deadline_us_;
            if (deadline != 0 && esp_timer_get_time() >= deadline) {
                look_at_deadline_us_ = 0;
                GoHome();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void StackChanBody::SetCaptureQuiet(bool quiet) {
    if (!ready_ || !ioe_) return;
    MutexGuard lock(motion_mutex_);
    if (quiet == capture_quiet_) return;
    capture_quiet_ = quiet;
    if (!kCaptureCutsRail) {
        gesture_abort_ = quiet ? true : gesture_abort_.load();
        if (quiet) {
            // Mic went hot: stop wherever the head is and keep the rail up. Nothing new
            // moves until capture closes (motion commands check capture_quiet_).
            look_at_deadline_us_ = 0;
            local_.Cancel();
            rest_baseline_valid_ = false;
            if (motion_) motion_->stop();
            ESP_LOGI(TAG, "capture hold: motion paused, servo rail kept");
        } else {
            ESP_LOGI(TAG, "capture released: motion allowed");
        }
        return;
    }
    if (quiet) {
        gesture_abort_ = true;
        // Mic went hot: cut torque (head limp) while the rail is still up, then drop the
        // servo power rail (PY32 VM EN). The rail's switching whine is the noise source;
        // powering it down is the only thing that silences it. Head droop while listening
        // is the accepted trade-off.
        look_at_deadline_us_ = 0;
        if (motion_) motion_->freeze();
        ioe_->digitalWrite(PY32_SERVO_POWER_PIN, false);
        ESP_LOGI(TAG, "capture quiet: servo rail off (silent mic)");
    } else {
        // Mic closed (agent speaking / not capturing): restore the rail so motion can
        // resume. Torque stays released until the next motion command re-engages it; the
        // servos need a short boot before they track again.
        ioe_->digitalWrite(PY32_SERVO_POWER_PIN, true);
        power_ready_us_ = esp_timer_get_time() + 200000;
        ESP_LOGI(TAG, "capture live: servo rail on");
    }
}

bool StackChanBody::GestureContinue(int delay_ms) {
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    return ready_ && !gesture_abort_ && !capture_quiet_;
}

void StackChanBody::SetHeadAngles(float yaw_deg, float pitch_deg, int speed) {
    if (!ready_ || capture_quiet_) return;  // rail down during capture -> head limp
    look_at_deadline_us_ = 0;  // any explicit angle move supersedes a look_at TTL hold
    MutexGuard lock(motion_mutex_);
    ServerMotionLocked();
    motion_->moveWithSpeed(static_cast<int>(std::lround(yaw_deg * 10.0f)),
                           static_cast<int>(std::lround(pitch_deg * 10.0f)), speed);
}

void StackChanBody::LookAtNormalized(float x, float y, int speed, int ttl_ms) {
    if (!ready_ || capture_quiet_) return;  // rail down during capture -> head limp
    MutexGuard lock(motion_mutex_);
    ServerMotionLocked();
    // y = 0 looks at rest height (as it did when rest was mid-range); the halves
    // below and above rest each span to their own limit.
    const float yc = std::clamp(y, -1.0f, 1.0f);
    const float pitch = yc >= 0 ? kHomePitchDeg + yc * (kPitchTopDeg - kHomePitchDeg)
                                : kHomePitchDeg + yc * (kHomePitchDeg - kPitchFloorDeg);
    const float yaw = std::clamp(x, -1.0f, 1.0f) * SERVO_YAW_MAX_DEG;
    motion_->moveWithSpeed(static_cast<int>(std::lround(yaw * 10.0f)),
                           static_cast<int>(std::lround(pitch * 10.0f)), speed);
    // Arm the return-home guardrail; 0 = hold until the next command moves the head.
    look_at_deadline_us_ = ttl_ms > 0 ? esp_timer_get_time() + static_cast<int64_t>(ttl_ms) * 1000 : 0;
}

void StackChanBody::GoHome(int speed) {
    SetHeadAngles(kHomeYawDeg, kHomePitchDeg, speed);  // clears the TTL hold via SetHeadAngles
}

void StackChanBody::Stop(bool latch) {
    if (!ready_) return;
    gesture_abort_ = true;  // set before the lock so a mid-step gesture bails out
    MutexGuard lock(motion_mutex_);
    look_at_deadline_us_ = 0;
    ServerMotionLocked();   // also ends an earlier latch
    if (latch) {
        local_.SafetyStop(esp_timer_get_time());
        ESP_LOGW(TAG, "safety stop: local head clips off until the next server head command or session end");
    }
    // While quiet the rail is down and the head is already limp; skip the bus write.
    if (!capture_quiet_ && motion_) {
        motion_->freeze();  // freeze animation + cut torque -> head limp (safety.stop = 断使能)
    }
}

void StackChanBody::SetAllLeds(uint8_t r, uint8_t g, uint8_t b) {
    if (!ioe_) return;
    for (int i = 0; i < kLedCount; ++i) ioe_->setLedColor(i, r, g, b);
    ioe_->refreshLeds();
}

void StackChanBody::RgbOff() {
    if (pet_leds_) return;   // the pet runtime owns the ring
    rgb_abort_ = true;  // preempt any running marquee before clearing
    SetAllLeds(0, 0, 0);
}

void StackChanBody::RgbMarquee() {
    if (!ioe_ || rgb_busy_ || pet_leds_) return;  // absent, running, or owned by the pet
    rgb_abort_ = false;
    rgb_busy_ = true;
    xTaskCreate(
        [](void* arg) {
            auto* b = static_cast<StackChanBody*>(arg);
            b->RunRgbMarquee();
            b->rgb_busy_ = false;
            vTaskDelete(nullptr);
        },
        "stackchan_rgb", 3072, this, 3, nullptr);
}

void StackChanBody::RunRgbMarquee() {
    // Cyan dot chasing around the ring with a short fading tail, a few laps, then dark.
    const int laps = 3;
    for (int step = 0; step < kLedCount * laps && !rgb_abort_; ++step) {
        const int head = step % kLedCount;
        for (int i = 0; i < kLedCount; ++i) {
            const int back = (head - i + kLedCount) % kLedCount;  // 0 = head, higher = tail
            uint8_t level = 0;
            if (back == 0) level = 255;
            else if (back == 1) level = 90;
            else if (back == 2) level = 25;
            // Tech cyan: green ~0.7 of blue.
            ioe_->setLedColor(i, static_cast<uint8_t>(0),
                              static_cast<uint8_t>(level * 7 / 10), level);
        }
        ioe_->refreshLeds();
        vTaskDelay(pdMS_TO_TICKS(45));
    }
    SetAllLeds(0, 0, 0);
}

void StackChanBody::SelfTest() {
    ESP_LOGI(TAG, "self-test sweep begin");
    const int kSettleMs = 1500;
    GoHome();                                 vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    SetHeadAngles(30.0f, kHomePitchDeg);      vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // right
    SetHeadAngles(-30.0f, kHomePitchDeg);     vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // left
    SetHeadAngles(kHomeYawDeg, kHomePitchDeg); vTaskDelay(pdMS_TO_TICKS(kSettleMs)); // center
    SetHeadAngles(kHomeYawDeg, RestPitch(15.0f)); vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // up
    SetHeadAngles(kHomeYawDeg, RestPitch(-4.0f)); vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // down
    GoHome();                                 vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    // Gesture demo — verifies the gesture sequences on boot (reuses RunGesture; safe
    // here because no async gesture runs during the boot self-test).
    gesture_.name = "nod";   gesture_.times = 2; RunGesture();
    gesture_.name = "shake"; gesture_.times = 2; RunGesture();
    ESP_LOGI(TAG, "self-test sweep done");
}

void StackChanBody::StartSelfTest() {
    if (!ready_) return;
    xTaskCreate(
        [](void* arg) {
            static_cast<StackChanBody*>(arg)->SelfTest();
            vTaskDelete(nullptr);
        },
        "stackchan_selftest", 4096, this, 3, nullptr);
}

void StackChanBody::HeadGesture(const std::string& name, int times, float x, float y,
                                int hold_ms, int return_ms, eidolon::HeadMotionObserver observer) {
    using S = eidolon::HeadMotionStatus;
    const bool known = name=="nod" || name=="shake" || name=="perk_up" ||
        name=="droop" || name=="glance" || name=="wake_wobble" || name=="home";
    const char* refusal = !ready_ ? "NO_HEAD_MOTION" : !known ? "UNKNOWN_GESTURE" :
        (times<0 || times>3 || hold_ms<0 || hold_ms>2000 || return_ms<0 || return_ms>2000 ||
         !std::isfinite(x) || !std::isfinite(y)) ? "INVALID_ARGUMENT" :
        capture_quiet_ ? "CAPTURING" : nullptr;
    if (refusal) { if (observer) observer({S::Rejected, refusal}); return; }
    bool idle=false;
    if (!gesture_busy_.compare_exchange_strong(idle,true)) {
        if (observer) observer({S::Rejected,"BUSY"});
        return;
    }
    {
        MutexGuard lock(motion_mutex_);
        ServerMotionLocked();   // a server gesture preempts any local clip at once
    }
    gesture_observer_=std::move(observer);
    gesture_.name = name;
    gesture_.times = times;
    gesture_.x = x;
    gesture_.y = y;
    gesture_.hold_ms = hold_ms;
    gesture_.return_ms = return_ms;
    gesture_abort_ = false;  // fresh gesture clears any prior safety-stop request
    const auto created=xTaskCreate(
        [](void* arg) {
            auto* b = static_cast<StackChanBody*>(arg);
            using S=eidolon::HeadMotionStatus;
            const auto remaining=b->power_ready_us_.load()-esp_timer_get_time();
            if (remaining>0) vTaskDelay(pdMS_TO_TICKS((remaining+999)/1000));
            if (!b->gesture_abort_ && !b->capture_quiet_) {
                if (b->gesture_observer_) b->gesture_observer_({S::Started,""});
                b->RunGesture();
                // Software sequence completion, not a claim of measured arrival.
                b->GestureContinue(250);
            }
            const bool cancelled=b->gesture_abort_ || b->capture_quiet_;
            auto observer=std::move(b->gesture_observer_);
            b->gesture_busy_=false;
            if (observer) observer({cancelled ? S::Cancelled : S::Completed,
                cancelled ? "STOPPED" : "SEQUENCE_FINISHED"});
            vTaskDelete(nullptr);
        },
        "stackchan_gesture", 4096, this, 3, nullptr);
    if (created!=pdPASS) {
        auto callback=std::move(gesture_observer_);gesture_busy_=false;
        if (callback) callback({S::Failed,"TASK_UNAVAILABLE"});
    }
}

void StackChanBody::RunGesture() {
    const auto& g = gesture_;
    const int times = g.times > 0 ? g.times : 2;
    ESP_LOGI(TAG, "gesture: %s", g.name.c_str());
    // Each step checks GestureContinue after moving; a safety.stop mid-gesture returns
    // early WITHOUT homing, so the head stays limp instead of driving back to center.
    if (g.name == "home") {
        GoHome();
        GestureContinue(650);
    } else if (g.name == "nod") {  // pitch oscillation (yes)
        for (int i = 0; i < times; ++i) {
            // Rest is near the lower stop: a nod is a lift and a dip back past rest.
            SetHeadAngles(0.0f, RestPitch(12.0f)); if (!GestureContinue(280)) return;
            SetHeadAngles(0.0f, RestPitch(-3.0f)); if (!GestureContinue(280)) return;
        }
        GoHome();
    } else if (g.name == "shake") {  // yaw oscillation (no)
        for (int i = 0; i < times; ++i) {
            SetHeadAngles(-22.0f, kHomePitchDeg); if (!GestureContinue(260)) return;
            SetHeadAngles(22.0f, kHomePitchDeg);  if (!GestureContinue(260)) return;
        }
        GoHome();
    } else if (g.name == "perk_up") {  // wake: look up, then settle
        SetHeadAngles(0.0f, RestPitch(22.0f)); if (!GestureContinue(g.hold_ms > 0 ? g.hold_ms : 800)) return;
        GoHome();
    } else if (g.name == "droop") {  // fatigue mirror: droop low, hold, recover
        SetHeadAngles(0.0f, kPitchFloorDeg); if (!GestureContinue(g.hold_ms > 0 ? g.hold_ms : 1200)) return;
        GoHome();
    } else if (g.name == "glance") {  // look toward target, then return
        LookAtNormalized(g.x, g.y); if (!GestureContinue(g.return_ms > 0 ? g.return_ms : 800)) return;
        GoHome();
    } else if (g.name == "wake_wobble") {  // cute wake: perk up, quick side-to-side wiggle, settle
        SetHeadAngles(0.0f, RestPitch(17.0f));   if (!GestureContinue(200)) return;
        SetHeadAngles(-15.0f, RestPitch(15.0f)); if (!GestureContinue(140)) return;
        SetHeadAngles(15.0f, RestPitch(15.0f));  if (!GestureContinue(140)) return;
        SetHeadAngles(-9.0f, RestPitch(13.0f));  if (!GestureContinue(130)) return;
        SetHeadAngles(6.0f, RestPitch(13.0f));   if (!GestureContinue(130)) return;
        GoHome();
    } else {
        ESP_LOGW(TAG, "unknown gesture: %s", g.name.c_str());
    }
}

// ---- Desk pet ---------------------------------------------------------------------------

void StackChanBody::NoteMotionLocked(int64_t now_us, int64_t suspect_us) {
    last_motion_us_ = now_us;
    head_level_ = false;
    if (motion_observer_) motion_observer_(now_us + suspect_us);
}

void StackChanBody::SetMotionObserver(std::function<void(int64_t until_us)> observer) {
    MutexGuard lock(motion_mutex_);
    motion_observer_ = std::move(observer);
}

void StackChanBody::ServerMotionLocked() {
    rest_baseline_valid_ = false;
    const int64_t now = esp_timer_get_time();
    local_.ServerMotion(now);
    NoteMotionLocked(now, 1500 * 1000);
}

bool StackChanBody::PlayLocalClip(const eidolon::pet::HeadClip& clip) {
    if (!ready_ || !motion_ || clip.count == 0 || clip.count > clip.keys.size()) return false;
    if (capture_quiet_ || gesture_busy_) return false;
    MutexGuard lock(motion_mutex_);
    const int64_t now = esp_timer_get_time();
    stackchan::LocalHeadPlayer::Body body;
    body.capture_quiet = capture_quiet_;
    body.gesture_busy = gesture_busy_;
    body.look_at_armed = look_at_deadline_us_ != 0;
    body.power_ready = now >= power_ready_us_.load();
    if (const char* refusal = local_.Refusal(clip, now, body)) {
        ESP_LOGD(TAG, "local clip p%u refused: %s", clip.priority, refusal);
        return false;
    }
    StartLocalLocked(&clip, now);
    return true;
}

// Starts `clip` (nullptr: the return home the player asked for). Reads the servos once,
// only here: the player decides whether to start from them or from the last command.
void StackChanBody::StartLocalLocked(const eidolon::pet::HeadClip* clip, int64_t now_us) {
    const auto commanded = motion_->getCommandedAngles();
    const auto readback = motion_->getCurrentAngles();
    const int rest = static_cast<int>(kHomePitchDeg * 10);
    const stackchan::HeadPose c{static_cast<float>(commanded.x), static_cast<float>(commanded.y - rest)};
    const stackchan::HeadPose r{static_cast<float>(readback.x), static_cast<float>(readback.y - rest)};
    if (clip) local_.Start(*clip, now_us, c, r);
    else local_.StartHome(now_us, c, r);
    NoteMotionLocked(now_us, static_cast<int64_t>(local_.path_ms() * 1000.0f) + kLocalSuspectTailUs);
    rest_baseline_valid_ = false;
}

void StackChanBody::DriveLocalLocked(const stackchan::HeadPose& pose) {
    motion_->followAngles(static_cast<int>(std::lround(pose.yaw)),
                          static_cast<int>(std::lround(pose.pitch + kHomePitchDeg * 10.0f)));
}

void StackChanBody::SettleLocalClip() {
    if (!ready_ || !motion_) return;
    MutexGuard lock(motion_mutex_);
    if (capture_quiet_ || gesture_busy_) return;
    stackchan::HeadPose held;
    if (!local_.Hold(esp_timer_get_time(), held)) return;
    DriveLocalLocked(held);   // the pose already written; keeps the servos from finishing a spring
    ESP_LOGI(TAG, "local clip held in place (yaw=%d dpitch=%d tenths)", static_cast<int>(held.yaw),
             static_cast<int>(held.pitch));
}

void StackChanBody::StepLocalClipLocked(int64_t now_us) {
    const auto step = local_.Tick(now_us);
    if (step.drive) DriveLocalLocked(step.pose);
    if (step.home_due) StartLocalLocked(nullptr, now_us);
}

void StackChanBody::SensePushLocked(int64_t now_us) {
    if (!push_cb_ || now_us < next_push_sample_us_) return;
    next_push_sample_us_ = now_us + kPushSamplePeriodUs;
    if (local_.active() || gesture_busy_ || motion_->isMoving()) {
        NoteMotionLocked(now_us, 600 * 1000);
        rest_baseline_valid_ = false;
        rest_since_us_ = 0;
        return;
    }
    const auto a = motion_->getCurrentAngles();   // physical readback (tenths of a degree)
    if (!rest_baseline_valid_) {
        // The animation is done; let the servos settle before taking the rest pose.
        if (rest_since_us_ == 0) { rest_since_us_ = now_us; return; }
        if (now_us - rest_since_us_ < kRestSettleUs) return;
        rest_yaw_ = a.x; rest_pitch_ = a.y; rest_baseline_valid_ = true;
        // The head sags up to ~7 deg below its target under its own weight.
        head_level_ = std::abs(a.y - static_cast<int>(kHomePitchDeg * 10)) <= 100;
        // Readback vs rest: tells a sagging/blocked head from a tilted base when the
        // IMU's head angle looks off (PetSensors logs gravity every 30 s).
        ESP_LOGI(TAG, "settled: readback yaw=%d pitch=%d (tenths; rest pitch %d)", a.x, a.y,
                 static_cast<int>(kHomePitchDeg * 10));
        push_hits_ = 0; push_last_yaw_ = a.x;
        return;
    }
    const int dy = a.x - rest_yaw_, dp = a.y - rest_pitch_;
    const bool off = std::abs(dy) > kPushThresholdDd || std::abs(dp) > kPushThresholdDd;
    if (!pushed_) {
        push_hits_ = off ? push_hits_ + 1 : 0;
        if (push_hits_ >= 2) {
            pushed_ = true;
            push_stable_since_us_ = now_us;
            push_last_yaw_ = a.x;
            NoteMotionLocked(now_us, 1500 * 1000);
            ESP_LOGI(TAG, "head pushed by hand (dyaw=%d dpitch=%d)", dy, dp);
            push_cb_(true, dy >= 0 ? 1 : -1);
        }
        return;
    }
    // Pushed: released once the head stops moving (hand let go, or settled where it was put).
    if (std::abs(a.x - push_last_yaw_) > 15) { push_stable_since_us_ = now_us; NoteMotionLocked(now_us, 1000 * 1000); }
    push_last_yaw_ = a.x;
    if (now_us - push_stable_since_us_ >= kPushReleaseStableUs) {
        pushed_ = false;
        push_hits_ = 0;
        rest_baseline_valid_ = false;
        rest_since_us_ = 0;
        ESP_LOGI(TAG, "head released");
        push_cb_(false, 0);
    }
}

void StackChanBody::SetPushCallback(std::function<void(bool pushed, int dir)> callback) {
    MutexGuard lock(motion_mutex_);
    push_cb_ = std::move(callback);
}

void StackChanBody::SetPetLedFrame(const uint8_t rgb[36]) {
    if (!ioe_) return;
    if (!pet_leds_) {
        pet_leds_ = true;
        rgb_abort_ = true;   // stop a legacy marquee; it checks the flag every step
    }
    portENTER_CRITICAL(&led_mux_);
    std::memcpy(led_pending_.data(), rgb, led_pending_.size());
    led_dirty_ = true;
    portEXIT_CRITICAL(&led_mux_);
}

void StackChanBody::FlushPetLeds() {
    if (!pet_leds_ || !ioe_ || rgb_busy_) return;
    const int64_t now = esp_timer_get_time();
    if (now < next_led_flush_us_) return;
    std::array<uint8_t, 36> frame;
    bool dirty;
    portENTER_CRITICAL(&led_mux_);
    dirty = led_dirty_;
    frame = led_pending_;
    led_dirty_ = false;
    portEXIT_CRITICAL(&led_mux_);
    if (!dirty || (led_written_valid_ && frame == led_written_)) return;
    uint8_t ram[kLedCount * 2];
    for (int i = 0; i < kLedCount; ++i) {
        const uint8_t r = frame[i * 3], g = frame[i * 3 + 1], b = frame[i * 3 + 2];
        const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        ram[kPetLedMap[i] * 2] = static_cast<uint8_t>(v & 0xFF);
        ram[kPetLedMap[i] * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    // Two short transactions on the shared bus (data, then refresh); no read-back.
    ioe_->setLedData(ram, sizeof(ram));
    ioe_->refreshLeds(kLedCount);
    led_written_ = frame;
    led_written_valid_ = true;
    next_led_flush_us_ = now + kLedFlushPeriodUs;
}
