#ifndef EIDOLON_ESP_IDF_CLAIM_GRANT_CRYPTO_H_
#define EIDOLON_ESP_IDF_CLAIM_GRANT_CRYPTO_H_

#include "device_claim_consumer_core.h"

#include <string>

namespace eidolon {

// ESP-IDF implementation of the frozen eidolon-trust-p256-hpke-v1 device
// boundary. The one-time handoff private key is durable only while an
// Enrollment journal can need it; DeviceIdentity remains the operational key.
class EspIdfClaimGrantCrypto final : public ClaimGrantCryptoPort {
public:
    // Terminal cleanup and pending resume must never create a replacement key.
    bool EnsureEnrollmentMaterial(bool create_if_missing = true);
    std::string HandoffPublicKey() const;
    std::string OperationalPublicKey() const;

    std::string HandoffKeyId() const override;
    std::string OperationalKeyId() const override;
    bool BuildHandoffKeyProof(const std::string& enrollment_id,
                              uint64_t proposal_revision,
                              const std::string& collection_challenge,
                              std::string& proof) override;
    ClaimGrantUnsealResult OpenClaimGrant(
        const device_foundation::v1::ClaimGrantWireEnvelope& envelope,
        const std::string& canonical_aad,
        device_foundation::v1::ClaimGrant& plaintext) override;
    bool BuildOperationalKeyProof(
        const std::string& enrollment_id,
        const std::string& grant_id,
        const device_foundation::v1::DeviceRef& device_ref,
        std::string& proof) override;
    bool DestroyEnrollmentMaterial(const std::string& enrollment_id,
                                   const std::string& handoff_key_id) override;

private:
    bool LoadHandoffPrivateKey(std::string& pem) const;
    bool SignWithHandoff(const std::string& canonical,
                         std::string& signature) const;

    mutable std::string handoff_public_key_;
    mutable std::string handoff_key_id_;
};

}  // namespace eidolon

#endif  // EIDOLON_ESP_IDF_CLAIM_GRANT_CRYPTO_H_
