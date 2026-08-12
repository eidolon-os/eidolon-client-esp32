#ifndef EIDOLON_HUB_TRUST_STORE_H_
#define EIDOLON_HUB_TRUST_STORE_H_

#include <string>

#include <esp_err.h>

namespace eidolon {

// The certificate this device requires the Hub to present.
//
// A Host signs its own Hub certificate, so no public CA can vouch for it. The
// device is told which certificate to expect by the person commissioning it —
// the same act that gives it Wi-Fi — and that hand-off is the only thing that
// authorizes this device to trust this Host. Nothing on the network can add or
// replace what is stored here.
class HubTrustStore {
public:
    // Replace the trusted certificate. Only the commissioning path calls this:
    // a human with the Owner's device decided this Host is the right one.
    esp_err_t Save(const std::string& hub_id, const std::string& certificate_pem);

    // The PEM to verify the Hub against, or empty when this device has not been
    // commissioned for that Hub. Callers must fail closed on empty rather than
    // falling back to a public trust store, which cannot contain a Host.
    std::string Load(const std::string& hub_id) const;

    // The Hub this device was commissioned for, or empty when it has not been.
    // A device serves exactly one Host; anything else answering discovery is
    // not its Host, whatever it calls itself.
    std::string CommissionedHubId() const;

    void Clear();
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TRUST_STORE_H_
