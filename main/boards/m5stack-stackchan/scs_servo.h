#pragma once

// SCSCL-backed Servo implementation, ported from the factory StackChan firmware
// (main/hal/hal_servo.cpp) and de-hosted for eidolon:
//   - GetHAL().millis()  -> esp_timer
//   - mclog              -> ESP_LOG
//   - factory SCSCL singleton -> a bus reference passed in by the owner
//   - Settings (NVS)     -> this repo's identical xiaozhi Settings class
// Keeps the proven angle<->raw mapping, per-unit NVS zero calibration, and software
// stall protection. The PWM / continuous-rotation path (yaw only, unused for head
// pose) was dropped.

#include <esp_log.h>
#include <esp_timer.h>

#include <string>

#include "servo.h"     // stackchan::motion::Servo (spring base) + uitk
#include "SCSCL.h"
#include "settings.h"

namespace stackchan::motion {

struct ServoConfig_t {
    int id = -1;
    int defaultZeroPos = 0;
    uitk::Vector2i angleLimit;   // tenths of a degree
    uitk::Vector2i rawPosLimit;  // raw SCSCL units
    std::string settingNs;
    std::string settingZeroPositionKey;
    bool enableStallProtection = false;
};

class ScsServo : public Servo {
public:
    ScsServo(const ServoConfig_t& config, SCSCL& bus)
        : _config(config), _bus(bus), _runtime_raw_pos_limit(config.rawPosLimit) {}

    void init() override {
        reset_runtime_limits();
        set_angle_limit(_config.angleLimit);
        get_zero_pos_from_nvs();
        Servo::init();
    }

    void get_zero_pos_from_nvs() {
        _zero_pos = _config.defaultZeroPos;
        bool is_valid = false;
        {
            Settings settings(_config.settingNs, false);
            int nvs_zero_pos = settings.GetInt(_config.settingZeroPositionKey, -1);
            if (nvs_zero_pos >= _config.rawPosLimit.x && nvs_zero_pos <= _config.rawPosLimit.y) {
                _zero_pos = nvs_zero_pos;
                is_valid = true;
                ESP_LOGI(TAG, "id: %d get zero pos: %d from settings", _config.id, _zero_pos);
            } else {
                ESP_LOGW(TAG, "id: %d invalid zero pos: %d from settings", _config.id, nvs_zero_pos);
            }
        }
        if (!is_valid) {
            _zero_pos = _config.defaultZeroPos;
            ESP_LOGI(TAG, "id: %d override zero pos to default: %d", _config.id, _zero_pos);
            Settings settings(_config.settingNs, true);
            settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        }
    }

    void set_angle_impl(int angle) override {
        int mapped = _zero_pos + angle * 16 / 5 / 10;  // 1 step = 0.3125 deg
        mapped = uitk::clamp(mapped, _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);
        if (update_stall_protection(mapped)) {
            return;
        }
        _bus.WritePos(_config.id, mapped, 20, 0);
    }

    int getCurrentAngle() override {
        int current_pos = _bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            int fallback = uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            ESP_LOGW(TAG, "id: %d invalid current pos: %d, fallback angle: %d", _config.id, current_pos, fallback);
            return fallback;
        }
        return uitk::clamp(raw_pos_to_angle(current_pos), getAngleLimit().x, getAngleLimit().y);
    }

    bool is_moving_impl() override {
        return _bus.ReadMove(_config.id) != 0;
    }

    void setTorqueEnabled(bool enabled) override {
        Servo::setTorqueEnabled(enabled);
        _bus.EnableTorque(_config.id, enabled ? 1 : 0);
    }

    bool getTorqueEnabled() override {
        return _bus.ReadToqueEnable(_config.id) > 0;
    }

    void setCurrentAngleAsZero() override {
        int current_pos = _bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            ESP_LOGW(TAG, "id: %d invalid zero-cal pos: %d, keep %d", _config.id, current_pos, _zero_pos);
            return;
        }
        _zero_pos = current_pos;
        reset_runtime_limits();
        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        ESP_LOGI(TAG, "id: %d set zero pos: %d", _config.id, _zero_pos);
    }

    void resetZeroCalibration() override {
        _zero_pos = _config.defaultZeroPos;
        reset_runtime_limits();
        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        ESP_LOGI(TAG, "id: %d reset zero pos: %d", _config.id, _zero_pos);
    }

private:
    static constexpr const char* TAG = "ScsServo";

    ServoConfig_t _config;
    SCSCL& _bus;
    uitk::Vector2i _runtime_raw_pos_limit;
    int _zero_pos = 0;

    static constexpr uint32_t kStallFeedbackIntervalMs = 50;
    static constexpr int kStallMinTargetDeltaRaw = 8;
    static constexpr int kStallMaxPositionDeltaRaw = 1;
    static constexpr int kStallCurrentRiseThreshold = 80;
    static constexpr int kStallLoadRiseThreshold = 150;
    static constexpr int kStallCurrentAbsThreshold = 350;
    static constexpr int kStallLoadAbsThreshold = 650;
    static constexpr int kStallConfirmSamples = 2;

    uint32_t _last_stall_check_tick = 0;
    int _last_stall_raw_pos = 0;
    int _last_stall_current_abs = 0;
    int _last_stall_load_abs = 0;
    int _last_stall_direction = 0;
    int _stall_confirm_count = 0;
    bool _last_stall_feedback_valid = false;

    static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
    static int abs_int(int v) { return v < 0 ? -v : v; }

    bool is_raw_pos_valid(int raw_pos) const {
        return raw_pos >= _config.rawPosLimit.x && raw_pos <= _config.rawPosLimit.y;
    }

    int raw_pos_to_angle(int raw_pos) const {
        return (raw_pos - _zero_pos) * 5 * 10 / 16;
    }

    void reset_runtime_limits() {
        _runtime_raw_pos_limit = _config.rawPosLimit;
        set_angle_limit(_config.angleLimit);
        reset_stall_detection();
    }

    void reset_stall_detection() {
        _last_stall_feedback_valid = false;
        _last_stall_direction = 0;
        _stall_confirm_count = 0;
    }

    bool update_stall_protection(int target_raw_pos) {
        if (!_config.enableStallProtection) {
            return false;
        }
        const uint32_t now = now_ms();
        if (now - _last_stall_check_tick < kStallFeedbackIntervalMs) {
            return false;
        }
        _last_stall_check_tick = now;

        if (_bus.FeedBack(_config.id) < 0) {
            reset_stall_detection();
            return false;
        }
        const int current_raw_pos = _bus.ReadPos(-1);
        const int current_abs = abs_int(_bus.ReadCurrent(-1));
        const int load_abs = abs_int(_bus.ReadLoad(-1));
        if (!is_raw_pos_valid(current_raw_pos)) {
            reset_stall_detection();
            return false;
        }
        const int target_delta = target_raw_pos - current_raw_pos;
        if (abs_int(target_delta) < kStallMinTargetDeltaRaw) {
            reset_stall_detection();
            return false;
        }
        const int direction = target_delta > 0 ? 1 : -1;
        if (_last_stall_feedback_valid && direction == _last_stall_direction) {
            const int pos_delta = abs_int(current_raw_pos - _last_stall_raw_pos);
            const bool position_stuck = pos_delta <= kStallMaxPositionDeltaRaw;
            const bool current_spike = current_abs >= kStallCurrentAbsThreshold ||
                                       current_abs - _last_stall_current_abs >= kStallCurrentRiseThreshold;
            const bool load_spike = load_abs >= kStallLoadAbsThreshold ||
                                    load_abs - _last_stall_load_abs >= kStallLoadRiseThreshold;
            if (position_stuck && (current_spike || load_spike)) {
                _stall_confirm_count++;
            } else if (pos_delta > kStallMaxPositionDeltaRaw) {
                _stall_confirm_count = 0;
            }
        } else {
            _stall_confirm_count = 0;
        }
        _last_stall_raw_pos = current_raw_pos;
        _last_stall_current_abs = current_abs;
        _last_stall_load_abs = load_abs;
        _last_stall_direction = direction;
        _last_stall_feedback_valid = true;

        if (_stall_confirm_count < kStallConfirmSamples) {
            return false;
        }
        handle_stall(current_raw_pos, direction, current_abs, load_abs);
        return true;
    }

    void handle_stall(int raw_pos, int direction, int current_abs, int load_abs) {
        int angle = uitk::clamp(raw_pos_to_angle(raw_pos), _config.angleLimit.x, _config.angleLimit.y);
        auto angle_limit = getAngleLimit();
        if (direction > 0) {
            if (raw_pos < _runtime_raw_pos_limit.y) _runtime_raw_pos_limit.y = raw_pos;
            if (angle < angle_limit.y) angle_limit.y = angle;
        } else {
            if (raw_pos > _runtime_raw_pos_limit.x) _runtime_raw_pos_limit.x = raw_pos;
            if (angle > angle_limit.x) angle_limit.x = angle;
        }
        set_angle_limit(angle_limit);
        stop_motion_at_angle(angle);
        reset_stall_detection();
        _bus.WritePos(_config.id, raw_pos, 20, 0);
        ESP_LOGW(TAG, "id: %d stall: raw=%d angle=%d dir=%d cur=%d load=%d limit=[%d,%d]",
                 _config.id, raw_pos, angle, direction, current_abs, load_abs, angle_limit.x, angle_limit.y);
    }
};

}  // namespace stackchan::motion
