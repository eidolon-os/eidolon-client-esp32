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
constexpr unsigned kProbeFailuresBeforeUnavailable = 3;

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

    sensor_bus_ = sensor_bus;
    callback_ = std::move(callback);
    running_ = true;
    if (xTaskCreate(TaskEntry, "box3_radar", 3072, this, 3, &task_) != pdPASS) {
        running_ = false;
        callback_ = {};
        sensor_bus_ = nullptr;
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
            const bool detected = ProbeSensorDock();
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

bool Box3RadarPresence::ProbeSensorDock()
{
    const esp_err_t err =
        i2c_master_probe(sensor_bus_, SENSOR_DOCK_AHT30_ADDRESS, 50);
    if (err == ESP_OK) {
        ESP_LOGD(kTag, "Sensor dock detected");
        return true;
    }
    ESP_LOGD(kTag, "Sensor dock probe failed: %s", esp_err_to_name(err));
    return false;
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
