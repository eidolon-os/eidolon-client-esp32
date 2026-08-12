#ifndef EIDOLON_HUB_PINNED_HTTP_H_
#define EIDOLON_HUB_PINNED_HTTP_H_

#include <string>

#include <esp_err.h>

namespace eidolon {

// One HTTPS request to the Hub, verified against the certificate this device
// was commissioned with.
//
// The board's own Http abstraction exists to span Wi-Fi and cellular modems and
// offers no way to say which certificate to trust — it always presents the
// public CA bundle, which cannot contain a Host's self-signed leaf. Hub
// onboarding is a LAN conversation with a machine discovered over mDNS, so it
// runs on the IP stack's own client, where the trusted certificate is an
// argument.
struct HubHttpResponse {
    int status = 0;
    std::string body;
};

// Perform `method` against `url`, requiring the server to present a certificate
// that `certificate_pem` verifies. An empty PEM is rejected rather than falling
// back to any other trust: a device that was never commissioned for this Host
// must not talk to it.
esp_err_t HubHttpRequest(const std::string& method,
                         const std::string& url,
                         const std::string& certificate_pem,
                         const std::string& request_body,
                         HubHttpResponse& out);

}  // namespace eidolon

#endif  // EIDOLON_HUB_PINNED_HTTP_H_
