#ifndef EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
#define EIDOLON_HUB_ONBOARDING_PROTOCOL_H_

#include <string>

#include "hub_types.h"

namespace eidolon {

bool ParseOwnerDomainDescriptor(
    const std::string& body,
    device_foundation::v1::OwnerDomainDescriptor& out,
    std::string& canonical_signing_bytes);

std::string BuildDeviceManifestJson(const std::string& board_name, bool has_camera);

bool ParseEnrollmentReceiptResponse(const std::string& body,
                                    const HubOnboardingState& expected,
                                    HubEnrollmentReceipt& out);

bool ParseHandoffResponse(const std::string& body,
                          const std::string& expected_request_id,
                          const HubOnboardingState& state,
                          HubConfigStatus& status,
                          HubChannelAssignment& assignment,
                          device_foundation::v1::DeviceRef& device_ref);

bool ParseDeviceConfigurationResponse(
    const std::string& body,
    const std::string& expected_nonce,
    const ActiveClaimState& expected,
    HubConfigStatus& status,
    HubChannelAssignment& assignment);

bool ParseLiveKitBinding(const std::string& body, Esp32HubConfig& out);

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
