#ifndef EIDOLON_DEVICE_COMMISSIONING_H_
#define EIDOLON_DEVICE_COMMISSIONING_H_

#include <functional>
#include <string>

#include <esp_err.h>
#include <esp_http_server.h>

namespace eidolon {

// What a person hands this device when they set it up.
//
// A device has no screen to read a code from and no Owner session to ask, so
// everything it needs to reach exactly one Host arrives in one act: the Wi-Fi
// it should join, and the certificate that identifies the Host on it. That act
// is the authorization — after it, the device trusts one Host and nothing else
// on the network can change that.
//
// This listens only while the device is in configuration mode, on the
// device's own access point, and stops with it.
class DeviceCommissioningServer {
public:
    // Join Wi-Fi with the credentials just received. Supplied by the board so
    // this stays free of any particular Wi-Fi implementation; returns false if
    // the credentials do not work, which is reported back to the commissioner
    // instead of being written to flash.
    using WifiJoin = std::function<bool(const std::string& ssid, const std::string& password)>;

    DeviceCommissioningServer() = default;
    ~DeviceCommissioningServer();

    DeviceCommissioningServer(const DeviceCommissioningServer&) = delete;
    DeviceCommissioningServer& operator=(const DeviceCommissioningServer&) = delete;

    esp_err_t Start(WifiJoin join);
    void Stop();

    static int Port();

private:
    static esp_err_t HandleIdentity(httpd_req_t* request);
    static esp_err_t HandleCommission(httpd_req_t* request);

    httpd_handle_t server_ = nullptr;
    WifiJoin join_;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_COMMISSIONING_H_
