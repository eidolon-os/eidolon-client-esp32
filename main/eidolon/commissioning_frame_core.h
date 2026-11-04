#ifndef EIDOLON_COMMISSIONING_FRAME_CORE_H_
#define EIDOLON_COMMISSIONING_FRAME_CORE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace eidolon {

// The device side of the chunked frames that carry the Eidolon setup endpoints
// (SDK golden/protocomm-endpoint-chunking.json, DF-COMM-017). The frames sit in
// the plaintext of each Security 2 message, so every chunk is still encrypted by
// the session. Over BLE one exchange holds at most 512 bytes; the trust handover
// is several kilobytes and the descriptor close to one, so neither fits whole.
//
// A port of the SDK's reference ChunkedEndpoint, tested against the same
// scripted exchanges byte for byte. One server per endpoint per session; it
// holds at most one transfer, and the request buffer is allocated once at the
// size the first chunk announces (in PSRAM above the internal-allocation
// threshold), bounded by the endpoint's limit.

enum class EndpointRequestOutcome {
    Ready,
    Pending,
    Failed,
};

struct EndpointRequestResult {
    EndpointRequestOutcome outcome = EndpointRequestOutcome::Failed;
    std::string response;
};

// `request` is the whole request the moment its last chunk arrives, and null on
// each later GET while the answer is still Pending.
using EndpointRequestHandler = std::function<EndpointRequestResult(const std::string* request)>;

struct EndpointFrameReply {
    // The handler refused the request. There is no frame to send: the endpoint
    // fails the exchange, exactly as it does for an unframed request today.
    bool handler_failed = false;
    std::string frame;
};

// Whether these bytes are a frame rather than a whole unframed request. JSON
// never starts with the magic, so the two cannot be confused.
bool IsEndpointFrame(const std::uint8_t* data, std::size_t length);

// The request limit for a framed endpoint, or 0 for an endpoint that is never
// framed (the SDK's own prov-* endpoints, proto-ver).
std::size_t EndpointFrameRequestLimit(std::string_view endpoint);

class EndpointFrameServer {
public:
    explicit EndpointFrameServer(std::size_t request_limit);

    EndpointFrameReply Handle(const std::uint8_t* data, std::size_t length,
                              const EndpointRequestHandler& handler,
                              std::uint16_t retry_after_ms = 250);

    // Forget any transfer: the session changed, the link dropped, or the
    // window is closing.
    void Reset();
    bool Idle() const { return transfer_id_ == 0; }

private:
    EndpointFrameReply Put(std::uint16_t transfer_id, std::uint16_t length, std::uint32_t total,
                           std::uint32_t offset, const std::uint8_t* chunk,
                           std::size_t chunk_length, const EndpointRequestHandler& handler,
                           std::uint16_t retry_after_ms);
    EndpointFrameReply Get(std::uint16_t transfer_id, std::uint16_t length, std::uint32_t total,
                           std::uint32_t offset, std::size_t chunk_length,
                           const EndpointRequestHandler& handler, std::uint16_t retry_after_ms);
    EndpointFrameReply Respond(const EndpointRequestResult& result, std::uint16_t transfer_id,
                               std::uint16_t retry_after_ms);

    std::size_t limit_;
    std::uint16_t transfer_id_ = 0;
    std::uint32_t total_ = 0;
    std::string request_;
    std::string response_;
    bool have_response_ = false;
    bool pending_ = false;
    std::uint32_t delivered_ = 0;
};

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_FRAME_CORE_H_
