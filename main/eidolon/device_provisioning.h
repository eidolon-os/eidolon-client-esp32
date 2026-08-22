#ifndef EIDOLON_DEVICE_PROVISIONING_H_
#define EIDOLON_DEVICE_PROVISIONING_H_

#include "commissioning_transport_core.h"

#include <atomic>
#include <functional>
#include <string>

#include <esp_err.h>
#include <esp_event.h>
#include <wifi_provisioning/manager.h>

#include <vector>

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
// The Wi-Fi half of the act is the SDK's own endpoints. The Eidolon half is the
// canonical descriptor, trust, status and terminal-ack surface. This adapter
// owns the HTTP endpoint capacity and every session resource for one generation;
// SDK callbacks only copy evidence and never perform teardown.
class DeviceProvisioningService {
public:
    struct Events {
        std::function<esp_err_t(uint32_t, const uint8_t*, size_t,
                                std::string&, bool&)> stage_trust;
        std::function<void(uint32_t, const std::string&)> transport_ready;
        std::function<void(uint32_t)> authenticated_session_started;
        std::function<void(uint32_t, const std::string&, const std::string&)>
            network_candidate_received;
        std::function<void(uint32_t)> wifi_connected;
        std::function<void(uint32_t)> wifi_connection_failed;
        std::function<void(uint32_t)> window_expired;
        std::function<void(uint32_t)> transport_stopped;
        std::function<void(uint32_t)> transport_ended_unexpectedly;
        std::function<std::string(uint32_t)> commissioning_status;
        std::function<bool(uint32_t, const std::string&)> terminal_ack;
    };

    static DeviceProvisioningService& GetInstance();

    esp_err_t Start(uint32_t generation, std::string session_id, Events events);
    void Stop(uint32_t generation);

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

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
    static esp_err_t HandleStatus(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen,
                                  uint8_t** outbuf, ssize_t* outlen, void* priv_data);
    static esp_err_t HandleTerminalAck(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen,
                                       uint8_t** outbuf, ssize_t* outlen, void* priv_data);
    static void HandleProvisioningEvent(void* arg, const char* event_base, int32_t event_id,
                                        void* event_data);

    esp_err_t RegisterEventHandlers();
    esp_err_t StartOwnedHttpServer();
    esp_err_t StartTransport();
    void MaybeReportReady();
    void CleanupTransport(uint32_t generation);
    void ReleaseNetifs();

    std::atomic<bool> running_{false};
    std::atomic<bool> cleanup_in_progress_{false};
    std::atomic<uint32_t> transport_generation_{0};
    std::string session_id_;
    Events events_;
    std::atomic<bool> manager_started_{false};
    std::atomic<bool> endpoints_registered_{false};
    std::atomic<bool> ready_reported_{false};
    CommissioningTransportResourceCore resources_;
    esp_event_handler_instance_t provisioning_event_instance_ = nullptr;
    esp_event_handler_instance_t security_event_instance_ = nullptr;
    // The security parameters and the buffers they point at must stay alive until
    // provisioning ends, because protocomm reads them when a controller actually
    // connects — which is whenever a person gets around to it, long after the
    // call that started the service returned. Holding them as locals meant the
    // handshake authenticated against freed stack memory and every proof failed.
    std::vector<uint8_t> salt_;
    std::vector<uint8_t> verifier_;
    wifi_prov_security2_params_t security_params_ = {};
    void* sta_netif_ = nullptr;
    void* ap_netif_ = nullptr;
    void* httpd_handle_ = nullptr;
    void* window_timer_ = nullptr;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_PROVISIONING_H_
