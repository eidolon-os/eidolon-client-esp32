#ifndef EIDOLON_HUB_CONFIG_CLIENT_H_
#define EIDOLON_HUB_CONFIG_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "sdkconfig.h"
#include "hub_types.h"

namespace eidolon {

class HubConfigClient {
public:
    // ``session_intent`` (optional) declares why this voice session exists; when
    // non-empty it rides the ``X-Device-Session-Intent`` header so the Hub can
    // stamp it into the voice token's metadata (channel suppresses the welcome
    // on a proactive wake). Empty = a normal JOIN (Hub defaults user_initiated).
    esp_err_t Fetch(const std::string& config_url, const std::string& device_id,
                    Esp32HubConfig& out, const std::string& session_intent = "");
#if CONFIG_EIDOLON_GUARD_SERVICE
    // Signed self-registration for guard-capable devices. The response shape is
    // the same runtime config returned by /api/config.
    esp_err_t RegisterGuardDevice(const std::string& config_url,
                                  const std::string& register_url,
                                  const std::string& device_id, Esp32HubConfig& out);
    // Fetch GuardBinding-local runtime config with the same signed device
    // identity as /api/config. It never fetches persona or policy config.
    esp_err_t FetchGuardRuntime(const std::string& config_url, const std::string& device_id,
                                GuardRuntimeHubConfig& out);
    esp_err_t FetchOwnerFaceProfile(const std::string& config_url,
                                    const std::string& device_id,
                                    OwnerFaceProfileHubConfig& out);
    esp_err_t FetchOwnerFaceReference(const std::string& config_url,
                                      const std::string& device_id,
                                      const OwnerFaceReferenceHubConfig& reference,
                                      std::string& out);
#endif

    bool HasPendingFirmware() const { return has_pending_firmware_; }
    bool PendingFirmwareForce() const { return pending_firmware_force_; }
    const std::string& PendingFirmwareVersion() const { return pending_firmware_version_; }
    const std::string& PendingFirmwareUrl() const { return pending_firmware_url_; }

private:
    bool has_pending_firmware_ = false;
    bool pending_firmware_force_ = false;
    std::string pending_firmware_version_;
    std::string pending_firmware_url_;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_CONFIG_CLIENT_H_
