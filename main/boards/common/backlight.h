#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>


class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    inline uint8_t brightness() const { return brightness_; }
    // The brightness chosen as a permanent setting, as distinct from the level
    // the screen is at now: power saving dims the screen without changing what
    // was chosen, and only a change of the choice is reported.
    inline uint8_t setting() const { return setting_; }
    // Told after the chosen brightness changes. Runs on the caller's task, so an
    // observer must only hand the value on. Set once, before anything changes it.
    void SetSettingObserver(std::function<void(uint8_t)> observer) {
        setting_observer_ = std::move(observer);
    }

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t setting_ = 0;
    std::function<void(uint8_t)> setting_observer_;
    uint8_t step_ = 1;
};


class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
