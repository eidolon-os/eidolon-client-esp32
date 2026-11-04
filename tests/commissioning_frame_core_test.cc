#include <cJSON.h>

#include <cassert>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eidolon/commissioning_frame_core.h"

namespace {

using eidolon::EndpointFrameReply;
using eidolon::EndpointFrameRequestLimit;
using eidolon::EndpointFrameServer;
using eidolon::EndpointRequestOutcome;
using eidolon::EndpointRequestResult;
using eidolon::IsEndpointFrame;

std::vector<std::uint8_t> FromHex(const std::string& hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string ToHex(const std::string& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

cJSON* ReadVector()
{
    std::ifstream file("tests/fixtures/device_foundation_v1/protocomm-endpoint-chunking.json");
    assert(file.good());
    std::stringstream text;
    text << file.rdbuf();
    cJSON* root = cJSON_Parse(text.str().c_str());
    assert(root != nullptr);
    return root;
}

// Replays one scripted exchange from the SDK vector against this device's core
// and requires every reply byte-for-byte. The same steps drive the SDK's
// reference endpoint and the phone's codec, so all three agree or one is red.
void Replay(const cJSON* scripted)
{
    const std::string endpoint = cJSON_GetObjectItemCaseSensitive(scripted, "endpoint")->valuestring;
    const std::string request = cJSON_GetObjectItemCaseSensitive(scripted, "request_utf8")->valuestring;
    const std::string response = cJSON_GetObjectItemCaseSensitive(scripted, "response_utf8")->valuestring;
    int polls = cJSON_GetObjectItemCaseSensitive(scripted, "pending_polls")->valueint;
    EndpointFrameServer server(EndpointFrameRequestLimit(endpoint));
    auto handler = [&](const std::string* whole) {
        if (whole != nullptr) assert(*whole == request);
        EndpointRequestResult result;
        if (polls > 0) {
            --polls;
            result.outcome = EndpointRequestOutcome::Pending;
        } else {
            result.outcome = EndpointRequestOutcome::Ready;
            result.response = response;
        }
        return result;
    };
    const cJSON* step = nullptr;
    cJSON_ArrayForEach(step, cJSON_GetObjectItemCaseSensitive(scripted, "steps")) {
        const auto sent = FromHex(cJSON_GetObjectItemCaseSensitive(step, "request_hex")->valuestring);
        const EndpointFrameReply reply = server.Handle(sent.data(), sent.size(), handler);
        assert(!reply.handler_failed);
        assert(ToHex(reply.frame) == cJSON_GetObjectItemCaseSensitive(step, "response_hex")->valuestring);
    }
}

void EveryScriptedExchangeIsAnsweredAsTheGoldenSays()
{
    cJSON* root = ReadVector();
    int replayed = 0;
    const cJSON* exchange = nullptr;
    cJSON_ArrayForEach(exchange, cJSON_GetObjectItemCaseSensitive(root, "exchanges")) {
        Replay(exchange);
        ++replayed;
    }
    const cJSON* refused = nullptr;
    cJSON_ArrayForEach(refused, cJSON_GetObjectItemCaseSensitive(root, "refused")) {
        Replay(refused);
        ++replayed;
    }
    assert(replayed == 12);
    cJSON_Delete(root);
}

// A handler that refuses fails the exchange with no frame, and the transfer is
// gone: the next transfer starts clean.
void ARefusingHandlerEndsTheTransferWithoutAFrame()
{
    EndpointFrameServer server(EndpointFrameRequestLimit("eidolon-status"));
    const std::vector<std::uint8_t> put = FromHex("e1d001010002000200000002000000007b7d");
    auto refuse = [](const std::string*) { return EndpointRequestResult{}; };
    const EndpointFrameReply reply = server.Handle(put.data(), put.size(), refuse);
    assert(reply.handler_failed && reply.frame.empty());
    assert(server.Idle());
}

void OnlyTheEidolonEndpointsAreFramed()
{
    assert(EndpointFrameRequestLimit("eidolon-trust") == 16 * 1024);
    assert(EndpointFrameRequestLimit("prov-config") == 0);
    const std::uint8_t json[] = {'{', '}'};
    assert(!IsEndpointFrame(json, sizeof(json)));
}

}  // namespace

int main()
{
    EveryScriptedExchangeIsAnsweredAsTheGoldenSays();
    ARefusingHandlerEndsTheTransferWithoutAFrame();
    OnlyTheEidolonEndpointsAreFramed();
    return 0;
}
