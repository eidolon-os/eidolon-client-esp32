#ifndef EIDOLON_DEVICE_PROVISIONING_H_
#define EIDOLON_DEVICE_PROVISIONING_H_

#include <functional>
#include <string>

#include <esp_err.h>

namespace eidolon {

// The ESP32 adapter for the Eidolon OS device provisioning contract.
//
// One setup act hands this device two things: the network to join and the Host
// to trust on it. What carries the act is protocomm — the same endpoints over
// BLE and over SoftAP — so this file is the only place that knows this device is
// an ESP32, and the contract above it knows nothing about which transport, which
// chip, or which vendor. A future device class is another adapter beside this
// one; nothing above has to change for it.
//
// The Wi-Fi half of the act is the SDK's own endpoints. The Eidolon half is two
// endpoints defined here: the descriptor a controller reads before it trusts
// anything, and the Host handover it writes. The order between them is the
// controller's to enforce, because the controller is the party that knows it.
class DeviceProvisioningService {
public:
    // Take the station back once provisioning has handed over credentials.
    // Supplied by the board so this stays free of any particular Wi-Fi
    // implementation: provisioning owns the radio while it runs and gives it
    // back when it stops, and only the board knows what "back" means.
    using StationHandover = std::function<void()>;

    static DeviceProvisioningService& GetInstance();

    esp_err_t Start(StationHandover handover);
    void Stop();

    bool IsRunning() const { return running_; }

private:
    DeviceProvisioningService() = default;

    // protocomm hands endpoint payloads to plain function pointers, so these
    // reach the instance through the singleton rather than through a capture.
    // The window this device told the controller about, enforced rather than
    // merely advertised. Without it a setup gesture leaves the radio in
    // provisioning mode until someone power-cycles the board, which is both a
    // worse recovery story than the one it replaced and an offer that stays
    // open to whoever is nearby.
    static void OnWindowElapsed(void* argument);

    static esp_err_t HandleDescriptor(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen,
                                      uint8_t** outbuf, ssize_t* outlen, void* priv_data);
    static esp_err_t HandleTrust(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen,
                                 uint8_t** outbuf, ssize_t* outlen, void* priv_data);
    static void HandleProvisioningEvent(void* arg, const char* event_base, int32_t event_id,
                                        void* event_data);

    esp_err_t StartTransport();
    void ReleaseNetifs();

    bool running_ = false;
    std::string session_id_;
    StationHandover handover_;
    void* sta_netif_ = nullptr;
    void* ap_netif_ = nullptr;
    void* window_timer_ = nullptr;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_PROVISIONING_H_
