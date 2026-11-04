#ifndef EIDOLON_SETUP_CODE_CARD_H_
#define EIDOLON_SETUP_CODE_CARD_H_

#include <string>
#include <string_view>

#include "eidolon/device_foundation_v1_generated.h"

namespace eidolon {

// What a device's screen shows while a setup window is open: the QR code a
// phone scans, the same eight digits for a person to type, and the name the
// device is advertising so the two can be matched. Held in RAM for one window
// and cleared with it; nothing here is ever persisted or logged.
struct SetupCodeCard {
    std::string code;
    std::string display_code;
    std::string advertised_name;
    device_foundation::v1::SetupQrTransport transport =
        device_foundation::v1::SetupQrTransport::Ble;
    std::string qr_json;

    bool empty() const { return code.empty(); }
    void Clear();
};

// The BLE device name for a setup window: "eidolon-<model>-<last six MAC hex
// digits>". The radio stack truncates a name past 29 bytes, and the part it
// drops first is the MAC suffix that tells two boards of one model apart, so
// each maintained board has a short model code here, and any other board's
// name is cut down to fit. SoftAP keeps its longer SSID; this is BLE only.
std::string BleSetupName(std::string_view board_type, std::string_view mac_address);

// The QR payload's canonical bytes (sorted keys, no whitespace), byte-for-byte
// what the SDK golden pins. Every value is restricted to characters JSON never
// escapes, which is what makes building it by hand safe.
std::string BuildSetupQrPayloadJson(const device_foundation::v1::SetupQrPayload& payload);

SetupCodeCard MakeSetupCodeCard(std::string code, std::string advertised_name,
                                device_foundation::v1::SetupQrTransport transport);

}  // namespace eidolon

#endif  // EIDOLON_SETUP_CODE_CARD_H_
