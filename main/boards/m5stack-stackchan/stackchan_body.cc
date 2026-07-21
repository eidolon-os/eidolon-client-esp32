#include "stackchan_body.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <cmath>

#include "config.h"
#include "PY32IOExpander_Class.hpp"
#include "motion.h"    // stackchan::motion::Motion
#include "scs_servo.h"  // stackchan::motion::ScsServo + ServoConfig_t

#define TAG "StackChanBody"

using stackchan::motion::Motion;
using stackchan::motion::ScsServo;
using stackchan::motion::ServoConfig_t;

namespace {
// Rest pose (degrees). Yaw centered; pitch mid-range (limits are 3deg..87deg).
constexpr float kHomeYawDeg = 0.0f;
constexpr float kHomePitchDeg = 45.0f;
constexpr int kDefaultSpeed = 500;
constexpr int kLedCount = 12;  // RGB ring on the PY32 IO-expander

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

StackChanBody::StackChanBody(i2c_master_bus_handle_t i2c_bus) : i2c_bus_(i2c_bus) {
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
        "stackchan_motion", 4096, this, 4, nullptr, 1);

    ESP_LOGI(TAG, "servo body ready");
    GoHome();
    return true;
}

void StackChanBody::UpdateLoop() {
    for (;;) {
        {
            MutexGuard lock(motion_mutex_);
            if (motion_) motion_->update();
        }
        // Guardrail: a look_at with ttl_ms returns home when its hold expires. Enforced
        // here (not in the caller) so no motion source can hold the head off-center past
        // its TTL. GoHome takes the mutex itself, so call it outside the block above.
        int64_t deadline = look_at_deadline_us_;
        if (deadline != 0 && esp_timer_get_time() >= deadline) {
            look_at_deadline_us_ = 0;
            GoHome();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool StackChanBody::GestureContinue(int delay_ms) {
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    return ready_ && !gesture_abort_;
}

void StackChanBody::SetHeadAngles(float yaw_deg, float pitch_deg, int speed) {
    if (!ready_) return;
    look_at_deadline_us_ = 0;  // any explicit angle move supersedes a look_at TTL hold
    MutexGuard lock(motion_mutex_);
    motion_->moveWithSpeed(static_cast<int>(std::lround(yaw_deg * 10.0f)),
                           static_cast<int>(std::lround(pitch_deg * 10.0f)), speed);
}

void StackChanBody::LookAtNormalized(float x, float y, int speed, int ttl_ms) {
    if (!ready_) return;
    MutexGuard lock(motion_mutex_);
    motion_->lookAtNormalized(x, y, speed);
    // Arm the return-home guardrail; 0 = hold until the next command moves the head.
    look_at_deadline_us_ = ttl_ms > 0 ? esp_timer_get_time() + static_cast<int64_t>(ttl_ms) * 1000 : 0;
}

void StackChanBody::GoHome(int speed) {
    SetHeadAngles(kHomeYawDeg, kHomePitchDeg, speed);  // clears the TTL hold via SetHeadAngles
}

void StackChanBody::Stop() {
    if (!ready_) return;
    gesture_abort_ = true;  // set before the lock so a mid-step gesture bails out
    MutexGuard lock(motion_mutex_);
    look_at_deadline_us_ = 0;
    motion_->freeze();  // freeze animation + cut torque -> head limp (safety.stop = 断使能)
}

void StackChanBody::SetAllLeds(uint8_t r, uint8_t g, uint8_t b) {
    if (!ioe_) return;
    for (int i = 0; i < kLedCount; ++i) ioe_->setLedColor(i, r, g, b);
    ioe_->refreshLeds();
}

void StackChanBody::RgbOff() {
    rgb_abort_ = true;  // preempt any running marquee before clearing
    SetAllLeds(0, 0, 0);
}

void StackChanBody::RgbMarquee() {
    if (!ioe_ || rgb_busy_) return;  // drop if the IO-expander is absent or one is running
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
    SetHeadAngles(kHomeYawDeg, 60.0f);        vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // up
    SetHeadAngles(kHomeYawDeg, 30.0f);        vTaskDelay(pdMS_TO_TICKS(kSettleMs));  // down
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
                                int hold_ms, int return_ms) {
    if (!ready_ || gesture_busy_) return;  // drop if a gesture is already running
    gesture_.name = name;
    gesture_.times = times;
    gesture_.x = x;
    gesture_.y = y;
    gesture_.hold_ms = hold_ms;
    gesture_.return_ms = return_ms;
    gesture_abort_ = false;  // fresh gesture clears any prior safety-stop request
    gesture_busy_ = true;
    xTaskCreate(
        [](void* arg) {
            auto* b = static_cast<StackChanBody*>(arg);
            b->RunGesture();
            b->gesture_busy_ = false;
            vTaskDelete(nullptr);
        },
        "stackchan_gesture", 4096, this, 3, nullptr);
}

void StackChanBody::RunGesture() {
    const auto& g = gesture_;
    const int times = g.times > 0 ? g.times : 2;
    ESP_LOGI(TAG, "gesture: %s", g.name.c_str());
    // Each step checks GestureContinue after moving; a safety.stop mid-gesture returns
    // early WITHOUT homing, so the head stays limp instead of driving back to center.
    if (g.name == "nod") {  // pitch oscillation (yes)
        for (int i = 0; i < times; ++i) {
            SetHeadAngles(0.0f, kHomePitchDeg - 12.0f); if (!GestureContinue(280)) return;
            SetHeadAngles(0.0f, kHomePitchDeg + 10.0f); if (!GestureContinue(280)) return;
        }
        GoHome();
    } else if (g.name == "shake") {  // yaw oscillation (no)
        for (int i = 0; i < times; ++i) {
            SetHeadAngles(-22.0f, kHomePitchDeg); if (!GestureContinue(260)) return;
            SetHeadAngles(22.0f, kHomePitchDeg);  if (!GestureContinue(260)) return;
        }
        GoHome();
    } else if (g.name == "perk_up") {  // wake: look up, then settle
        SetHeadAngles(0.0f, 72.0f); if (!GestureContinue(g.hold_ms > 0 ? g.hold_ms : 800)) return;
        GoHome();
    } else if (g.name == "droop") {  // fatigue mirror: droop low, hold, recover
        SetHeadAngles(0.0f, 8.0f); if (!GestureContinue(g.hold_ms > 0 ? g.hold_ms : 1200)) return;
        GoHome();
    } else if (g.name == "glance") {  // look toward target, then return
        LookAtNormalized(g.x, g.y); if (!GestureContinue(g.return_ms > 0 ? g.return_ms : 800)) return;
        GoHome();
    } else if (g.name == "wake_wobble") {  // cute wake: perk up, quick side-to-side wiggle, settle
        SetHeadAngles(0.0f, 62.0f);   if (!GestureContinue(200)) return;
        SetHeadAngles(-15.0f, 60.0f); if (!GestureContinue(140)) return;
        SetHeadAngles(15.0f, 60.0f);  if (!GestureContinue(140)) return;
        SetHeadAngles(-9.0f, 58.0f);  if (!GestureContinue(130)) return;
        SetHeadAngles(6.0f, 58.0f);   if (!GestureContinue(130)) return;
        GoHome();
    } else {
        ESP_LOGW(TAG, "unknown gesture: %s", g.name.c_str());
    }
}
