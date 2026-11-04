#include "eidolon/setup_code_card.h"

#include <cctype>

#include "eidolon/setup_code_secret.h"

namespace eidolon {
namespace {

namespace df = device_foundation::v1;

constexpr std::size_t kMaxModelCode = 14;

struct BoardCode {
    const char* board_type;
    const char* code;
};

// One short code per maintained board. Each is distinct, so two different
// models can never advertise the same name for the same MAC suffix.
constexpr BoardCode kBoardCodes[] = {
    {"esp-box-3", "box3"},
    {"m5stack-core-s3", "cores3"},
    {"m5stack-stackchan", "stackchan"},
    {"esp32-s3-touch-amoled-2.06", "amoled206"},
    {"korvo-1", "korvo1"},
    {"xiaoling", "xiaoling"},
    {"atk-dnesp32s3", "atks3"},
};

std::string ModelCode(std::string_view board_type)
{
    for (const auto& entry : kBoardCodes) {
        if (board_type == entry.board_type) return entry.code;
    }
    for (std::string_view prefix : {"esp32-s3-", "esp32-c6-", "esp32s3-", "esp32-"}) {
        if (board_type.substr(0, prefix.size()) == prefix) {
            board_type.remove_prefix(prefix.size());
            break;
        }
    }
    std::string code;
    for (char c : board_type) {
        const auto ch = static_cast<unsigned char>(c);
        if (std::isalnum(ch)) code.push_back(static_cast<char>(std::tolower(ch)));
        if (code.size() == kMaxModelCode) break;
    }
    return code.empty() ? "device" : code;
}

std::string MacTail(std::string_view mac_address)
{
    std::string hex;
    for (char c : mac_address) {
        const auto ch = static_cast<unsigned char>(c);
        if (std::isxdigit(ch)) hex.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (hex.size() < 6) hex.insert(0, 6 - hex.size(), '0');
    return hex.substr(hex.size() - 6);
}

}  // namespace

void SetupCodeCard::Clear()
{
    for (std::string* field : {&code, &display_code, &qr_json}) {
        for (char& c : *field) c = '\0';
        field->clear();
    }
    advertised_name.clear();
}

std::string BleSetupName(std::string_view board_type, std::string_view mac_address)
{
    return "eidolon-" + ModelCode(board_type) + "-" + MacTail(mac_address);
}

std::string BuildSetupQrPayloadJson(const df::SetupQrPayload& payload)
{
    std::string json;
    json.reserve(128);
    json += "{\"name\":\"";
    json += payload.name;
    json += "\",\"pop\":\"";
    json += payload.pop;
    json += "\",\"security\":";
    json += std::to_string(df::SetupQrPayload::kSecurity);
    json += ",\"transport\":\"";
    json += df::SetupQrTransportWireValue(payload.transport);
    json += "\",\"username\":\"";
    json += df::SetupQrPayload::kUsername;
    json += "\",\"ver\":\"";
    json += df::SetupQrPayload::kVersion;
    json += "\"}";
    return json;
}

SetupCodeCard MakeSetupCodeCard(std::string code, std::string advertised_name,
                                df::SetupQrTransport transport)
{
    SetupCodeCard card;
    df::SetupQrPayload payload;
    payload.name = advertised_name;
    payload.pop = code;
    payload.transport = transport;
    card.qr_json = BuildSetupQrPayloadJson(payload);
    payload.pop.assign(payload.pop.size(), '\0');
    card.display_code = FormatSetupCodeForDisplay(code);
    card.code = std::move(code);
    card.advertised_name = std::move(advertised_name);
    card.transport = transport;
    return card;
}

}  // namespace eidolon
