#ifndef EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
#define EIDOLON_HUB_ONBOARDING_PROTOCOL_H_

#include <string>

#include "hub_types.h"

namespace eidolon {

bool ParseHubDescriptorResponse(const std::string& body,
                                const HubTxtRecord& advertised,
                                HubDescriptor& out);

std::string BuildDeviceManifestJson(const std::string& board_name);

std::string BuildEnrollmentProofStatement(
    const std::string& request_id,
    const std::string& device_id,
    const std::string& retrieval_token_hash,
    const std::string& pairing_commitment,
    const std::string& device_kind,
    const std::string& display_name,
    const std::string& manifest_revision);

bool ParseEnrollmentReceiptResponse(const std::string& body,
                                    const HubOnboardingState& expected,
                                    HubEnrollmentReceipt& out);

bool ParseHandoffResponse(const std::string& body,
                          const std::string& expected_request_id,
                          const HubOnboardingState& state,
                          HubConfigStatus& status,
                          HubChannelAssignment& assignment);

bool ParseLiveKitBinding(const std::string& body, Esp32HubConfig& out);

std::string BuildLocalPairingPayload(const HubDescriptor& descriptor,
                                     const HubOnboardingState& state);

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
