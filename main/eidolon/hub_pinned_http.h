#ifndef EIDOLON_HUB_PINNED_HTTP_H_
#define EIDOLON_HUB_PINNED_HTTP_H_

#include <cstdint>
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
    // What the Hub's own clock read when it answered, from this response's
    // `Date` header, or zero when it did not state a time this device can read.
    //
    // A device in HUB_MODE has no other trusted reading: nothing in that build
    // ever sets the system clock, so `gettimeofday` answers 1970 for the whole
    // life of the boot. The Hub is the party that issues deadlines, this
    // response arrived over the certificate the device was commissioned with,
    // and so the Hub's own clock is the one reading a deadline it wrote can be
    // judged against. Zero means "no answer", never "the epoch".
    int64_t hub_utc_millis = 0;
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
