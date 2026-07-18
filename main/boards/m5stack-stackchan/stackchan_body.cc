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
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void StackChanBody::SetHeadAngles(float yaw_deg, float pitch_deg, int speed) {
    if (!ready_) return;
    MutexGuard lock(motion_mutex_);
    motion_->moveWithSpeed(static_cast<int>(std::lround(yaw_deg * 10.0f)),
                           static_cast<int>(std::lround(pitch_deg * 10.0f)), speed);
}

void StackChanBody::LookAtNormalized(float x, float y, int speed) {
    if (!ready_) return;
    MutexGuard lock(motion_mutex_);
    motion_->lookAtNormalized(x, y, speed);
}

void StackChanBody::GoHome(int speed) {
    SetHeadAngles(kHomeYawDeg, kHomePitchDeg, speed);
}

void StackChanBody::Stop() {
    if (!ready_) return;
    MutexGuard lock(motion_mutex_);
    motion_->stop();
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
    GoHome();
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
