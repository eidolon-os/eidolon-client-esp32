#ifndef EIDOLON_HUB_CONFIG_CLIENT_H_
#define EIDOLON_HUB_CONFIG_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "hub_types.h"

namespace eidolon {

class HubConfigClient {
public:
    esp_err_t Fetch(const std::string& config_url, const std::string& device_id, Esp32HubConfig& out);

    bool HasPendingFirmware() const { return has_pending_firmware_; }
    const std::string& PendingFirmwareVersion() const { return pending_firmware_version_; }
    const std::string& PendingFirmwareUrl() const { return pending_firmware_url_; }

private:
    bool has_pending_firmware_ = false;
    std::string pending_firmware_version_;
    std::string pending_firmware_url_;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_CONFIG_CLIENT_H_
