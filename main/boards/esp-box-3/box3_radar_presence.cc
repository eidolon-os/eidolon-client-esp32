#include "box3_radar_presence.h"

#include <utility>

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>

#include "config.h"

namespace {

constexpr char kTag[] = "Box3Radar";
constexpr uint32_t kPollIntervalMs = 100;
constexpr uint32_t kDockProbeIntervalMs = 5000;
constexpr uint32_t kI2cTimeoutMs = 100;
constexpr unsigned kProbeFailuresBeforeUnavailable = 3;

// AT581X register definitions used by Espressif's official driver.
constexpr uint8_t kRegSoftwareReset = 0x00;
constexpr uint8_t kRegSignalThresholdLow = 0x10;
constexpr uint8_t kRegSelfCheckLow = 0x38;
constexpr uint8_t kRegTriggerBase0 = 0x3D;
constexpr uint8_t kRegTriggerOutputControl = 0x41;
constexpr uint8_t kRegTriggerKeep0 = 0x42;
constexpr uint8_t kRegProtectionLow = 0x4E;
constexpr uint8_t kRegApplyConfiguration = 0x55;
constexpr uint8_t kRegReceiveGain = 0x5C;
constexpr uint8_t kRegWorkTime = 0x67;
constexpr uint8_t kRegBurstTime = 0x68;

constexpr uint16_t kSelfCheckMs = 2000;
constexpr uint16_t kTriggerBaseMs = 500;
constexpr uint16_t kTriggerKeepMs = 1500;
constexpr uint16_t kProtectionMs = 1000;
constexpr uint8_t kPower70Ua = 0x31;
constexpr uint8_t kGainStage3 = 3;

uint64_t NowMs()
{
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

}  // namespace

Box3RadarPresence::~Box3RadarPresence()
{
    running_ = false;
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (radar_device_ != nullptr) {
        i2c_master_bus_rm_device(radar_device_);
        radar_device_ = nullptr;
    }
}

esp_err_t Box3RadarPresence::Start(i2c_master_bus_handle_t sensor_bus,
                                   StateCallback callback)
{
    if (sensor_bus == nullptr || !callback || running_) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_config = {};
    io_config.pin_bit_mask = 1ULL << RADAR_PRESENCE_GPIO;
    io_config.mode = GPIO_MODE_INPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t radar_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SENSOR_DOCK_AT581X_ADDRESS,
        .scl_speed_hz = 100 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    err = i2c_master_bus_add_device(sensor_bus, &radar_config, &radar_device_);
    if (err != ESP_OK) {
        return err;
    }

    sensor_bus_ = sensor_bus;
    callback_ = std::move(callback);
    running_ = true;
    if (xTaskCreate(TaskEntry, "box3_radar", 3072, this, 3, &task_) != pdPASS) {
        running_ = false;
        callback_ = {};
        sensor_bus_ = nullptr;
        i2c_master_bus_rm_device(radar_device_);
        radar_device_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Box3RadarPresence::TaskEntry(void* arg)
{
    auto* self = static_cast<Box3RadarPresence*>(arg);
    self->Run();
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void Box3RadarPresence::Run()
{
    uint64_t last_probe_ms = 0;
    PublishState(RadarPresenceState::Unavailable);

    while (running_) {
        const uint64_t now_ms = NowMs();
        if (last_probe_ms == 0 || now_ms - last_probe_ms >= kDockProbeIntervalMs) {
            last_probe_ms = now_ms;
            bool detected = ProbeRadar();
            if (!detected) {
                radar_configured_ = false;
            } else if (!radar_configured_) {
                // A recovered or newly powered radar needs its volatile
                // registers restored and another self-check interval.
                tracker_.SetAvailable(false, now_ms);
                const esp_err_t err = ConfigureRadar();
                if (err == ESP_OK) {
                    radar_configured_ = true;
                    ESP_LOGI(kTag,
                             "configured threshold_delta=%d "
                             "(larger means shorter range)",
                             CONFIG_EIDOLON_BOX3_RADAR_THRESHOLD_DELTA);
                } else {
                    detected = false;
                    ESP_LOGW(kTag, "configuration failed: %s",
                             esp_err_to_name(err));
                }
            }
            if (detected) {
                consecutive_probe_failures_ = 0;
                tracker_.SetAvailable(true, now_ms);
            } else {
                ++consecutive_probe_failures_;
                if (!tracker_.available() ||
                    consecutive_probe_failures_ >= kProbeFailuresBeforeUnavailable) {
                    tracker_.SetAvailable(false, now_ms);
                }
            }
        }

        const bool raw_detected =
            tracker_.available() && gpio_get_level(RADAR_PRESENCE_GPIO) != 0;
        if (!has_raw_detected_ || raw_detected_ != raw_detected) {
            raw_detected_ = raw_detected;
            has_raw_detected_ = true;
            ESP_LOGI(kTag, "raw detection=%d", raw_detected);
        }
        PublishState(tracker_.Update(raw_detected, now_ms));
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

bool Box3RadarPresence::ProbeRadar()
{
    const esp_err_t err =
        i2c_master_probe(sensor_bus_, SENSOR_DOCK_AT581X_ADDRESS, 50);
    if (err == ESP_OK) {
        ESP_LOGD(kTag, "AT581X detected");
        return true;
    }
    ESP_LOGD(kTag, "AT581X probe failed: %s", esp_err_to_name(err));
    return false;
}

esp_err_t Box3RadarPresence::ConfigureRadar()
{
    const uint16_t threshold = CONFIG_EIDOLON_BOX3_RADAR_THRESHOLD_DELTA;
    const uint8_t burst_time =
        (1U << 5) | ((kPower70Ua & 0x0F) << 3) | (1U << 6);
    const uint8_t work_time =
        (1U << 3) | (kPower70Ua >> 4) | (1U << 7);

    const auto write16 = [this](uint8_t low_reg, uint16_t value) {
        esp_err_t err =
            WriteRadarRegister(low_reg, static_cast<uint8_t>(value));
        if (err != ESP_OK) {
            return err;
        }
        return WriteRadarRegister(low_reg + 1,
                                  static_cast<uint8_t>(value >> 8));
    };
    const auto write32 = [this](uint8_t low_reg, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            const esp_err_t err = WriteRadarRegister(
                low_reg + byte, static_cast<uint8_t>(value >> (byte * 8)));
            if (err != ESP_OK) {
                return err;
            }
        }
        return ESP_OK;
    };

    esp_err_t err = WriteRadarRegister(kRegBurstTime, burst_time);
    if (err == ESP_OK) {
        err = WriteRadarRegister(kRegWorkTime, work_time);
    }
    if (err == ESP_OK) {
        err = write16(kRegSignalThresholdLow, threshold);
    }
    if (err == ESP_OK) {
        err = WriteRadarRegister(
            kRegReceiveGain,
            static_cast<uint8_t>(0x0B | (kGainStage3 << 3)));
    }
    if (err == ESP_OK) {
        err = write32(kRegTriggerBase0, kTriggerBaseMs);
    }
    if (err == ESP_OK) {
        err = WriteRadarRegister(kRegTriggerOutputControl, 0x01);
    }
    if (err == ESP_OK) {
        err = write32(kRegTriggerKeep0, kTriggerKeepMs);
    }
    if (err == ESP_OK) {
        err = write16(kRegSelfCheckLow, kSelfCheckMs);
    }
    if (err == ESP_OK) {
        err = write16(kRegProtectionLow, kProtectionMs);
    }
    if (err == ESP_OK) {
        err = WriteRadarRegister(kRegApplyConfiguration, 0x04);
    }
    if (err == ESP_OK) {
        err = WriteRadarRegister(kRegSoftwareReset, 0x00);
    }
    if (err == ESP_OK) {
        err = WriteRadarRegister(kRegSoftwareReset, 0x01);
    }
    return err;
}

esp_err_t Box3RadarPresence::WriteRadarRegister(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(radar_device_, data, sizeof(data),
                               kI2cTimeoutMs);
}

void Box3RadarPresence::PublishState(RadarPresenceState state)
{
    if (has_published_state_ && published_state_ == state) {
        return;
    }
    published_state_ = state;
    has_published_state_ = true;
    ESP_LOGI(kTag, "presence state=%s raw=%d", RadarPresenceStateName(state),
             gpio_get_level(RADAR_PRESENCE_GPIO));
    callback_(state);
}
