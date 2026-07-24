#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include <driver/i2c_master.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;

    /** Shared I2C bus for codecs (Eidolon LiveKit media). Default: none. */
    virtual i2c_master_bus_handle_t GetSharedI2cBus() { return nullptr; }

    /**
     * Head/body motion for boards with a servo body (e.g. StackChan). These are
     * discrete, brain/hub-driven gestures; boards without a body use the no-op
     * defaults. x,y are normalized [-1,1] (0,0 = centered/forward).
     */
    virtual bool HasHeadMotion() { return false; }
    // ttl_ms > 0 returns the head home when the hold expires (device guardrail).
    virtual void HeadLookAt(float x, float y, int speed, int ttl_ms) {
        (void)x; (void)y; (void)speed; (void)ttl_ms;
    }
    virtual void HeadHome() {}
    // Discrete expressive gesture: name in {nod, shake, perk_up, droop, glance}.
    virtual void HeadGesture(const std::string& name, int times, float x, float y,
                             int hold_ms, int return_ms) {
        (void)name; (void)times; (void)x; (void)y; (void)hold_ms; (void)return_ms;
    }
    // Emergency stop: cut head-servo torque and preempt any running gesture.
    virtual void HeadStop() {}

    // Mic-capture noise gate for boards whose motor/servo power rail whines into
    // the on-board mic (e.g. StackChan). `quiet=true` while the mic is hot for
    // uplink so the board can power the rail down (head goes limp) and keep the
    // captured audio clean; `quiet=false` when the mic is closed so motion can
    // resume. No-op on boards without a noisy body.
    virtual void SetCaptureQuiet(bool quiet) { (void)quiet; }

    // Board-local decorative feedback (RGB ring / screen avatar) used by the
    // owner-presence reflex. No-ops on boards without them. `effect`: "wake"
    // (marquee) / "off". `emotion`: a short-lived face pulse for ttl_ms.
    virtual void RgbEffect(const char* effect) { (void)effect; }
    virtual void AvatarExpress(const char* emotion, int ttl_ms) { (void)emotion; (void)ttl_ms; }
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
