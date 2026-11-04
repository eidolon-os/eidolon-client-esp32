#include <cJSON.h>

#include <cassert>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "eidolon/setup_code_card.h"

namespace {

namespace df = eidolon::device_foundation::v1;
using eidolon::BleSetupName;
using eidolon::BuildSetupQrPayloadJson;
using eidolon::MakeSetupCodeCard;

cJSON* ReadVector()
{
    std::ifstream file("tests/fixtures/device_foundation_v1/setup-qr-payload.json");
    assert(file.good());
    std::stringstream text;
    text << file.rdbuf();
    cJSON* root = cJSON_Parse(text.str().c_str());
    assert(root != nullptr);
    return root;
}

// The bytes this device encodes into its QR code are the bytes the SDK pins,
// for both transports. A key the phone did not know would be a code no phone
// can read; this is where that becomes a red test instead.
void TheQrBytesAreTheGoldenVectorBytes()
{
    cJSON* root = ReadVector();
    for (const char* shape : {"ble", "softap"}) {
        const cJSON* entry = cJSON_GetObjectItemCaseSensitive(root, shape);
        const cJSON* payload = cJSON_GetObjectItemCaseSensitive(entry, "payload");
        df::SetupQrPayload value;
        value.name = cJSON_GetObjectItemCaseSensitive(payload, "name")->valuestring;
        value.pop = cJSON_GetObjectItemCaseSensitive(payload, "pop")->valuestring;
        value.transport = std::string(shape) == "ble" ? df::SetupQrTransport::Ble
                                                      : df::SetupQrTransport::SoftAp;
        const std::string expected =
            cJSON_GetObjectItemCaseSensitive(entry, "canonical_utf8")->valuestring;
        assert(BuildSetupQrPayloadJson(value) == expected);
    }
    cJSON_Delete(root);
}

// Every maintained board gets a BLE name inside the 29 bytes the scan response
// carries, with its whole MAC suffix, and no two models share a name.
void EveryMaintainedBoardFitsTheScanResponse()
{
    const std::regex pattern("^eidolon-[a-z0-9]+-[0-9a-f]{6}$");
    std::set<std::string> names;
    for (const char* board : {"esp-box-3", "m5stack-core-s3", "m5stack-stackchan",
                              "esp32-s3-touch-amoled-2.06", "korvo-1", "xiaoling",
                              "atk-dnesp32s3"}) {
        const std::string name = BleSetupName(board, "24:EC:4A:52:F3:54");
        assert(name.size() <= df::BleProvisioningTransport::kDeviceNameMaxBytes);
        assert(std::regex_match(name, pattern));
        assert(name.substr(name.size() - 6) == "52f354");
        names.insert(name);
    }
    assert(names.size() == 7);
    assert(BleSetupName("esp-box-3", "24:ec:4a:52:f3:54") == "eidolon-box3-52f354");
}

void AnUnknownBoardIsCutDownRatherThanTruncatedOnAir()
{
    const std::string name =
        BleSetupName("esp32-s3-some-very-long-board-name-v2", "aa:bb:cc:dd:ee:ff");
    assert(name.size() <= df::BleProvisioningTransport::kDeviceNameMaxBytes);
    assert(name.substr(name.size() - 7) == "-ddeeff");
}

void TheCardCarriesTheSameCodeTwiceAndClearsItself()
{
    auto card = MakeSetupCodeCard("40718256", "eidolon-box3-a1b2c3", df::SetupQrTransport::Ble);
    assert(card.display_code == "4071 8256");
    assert(card.qr_json.find("\"pop\":\"40718256\"") != std::string::npos);
    card.Clear();
    assert(card.empty() && card.qr_json.empty() && card.display_code.empty());
}

}  // namespace

int main()
{
    TheQrBytesAreTheGoldenVectorBytes();
    EveryMaintainedBoardFitsTheScanResponse();
    AnUnknownBoardIsCutDownRatherThanTruncatedOnAir();
    TheCardCarriesTheSameCodeTwiceAndClearsItself();
    return 0;
}
