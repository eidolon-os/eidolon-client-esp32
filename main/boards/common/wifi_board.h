#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include "sdkconfig.h"

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void TryWifiConnect();

    /**
     * Open setup because the firmware decided to, with nobody present: a boot
     * that found no network profile, or CONNECT_TIMEOUT_SEC of Station failing
     * to associate.
     *
     * This is the gated door, and StartWifiConfigMode is not. Under HUB_MODE a
     * request nobody made may not open a window on a device that already has
     * an Owner — forbidden path D8 — so both automatic callers come through
     * here, and a board that wants setup on a button must call
     * EnterWifiConfigMode, which is the physical-presence door.
     */
    void OpenSetupWithoutAnyonePresent();

    /**
     * Enter WiFi configuration mode.
     *
     * The act itself, not a decision about whether it may happen: the caller
     * is asserting that somebody authorized this. EnterWifiConfigMode is the
     * physical-presence door and is what a board's button or touch gesture
     * should call; two boards (m5stack-core-s3, m5stack-stackchan) call this
     * directly instead, which is a pre-existing gap, not a pattern to copy.
     */
    void StartWifiConfigMode();

    /**
     * WiFi connection timeout callback
     */
    bool commissioning_recovery_pending_ = false;
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    std::optional<bool> IsNetworkConnected() const override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();

    
    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
