#ifndef SETUP_CODE_SCREEN_H_
#define SETUP_CODE_SCREEN_H_

#include <string>

// What the screen shows while a setup window is open and a code was drawn for
// it. Plain strings: the display layer knows nothing about commissioning.
struct SetupCodeScreen {
    std::string qr_text;        // the QR payload, exactly as the phone parses it
    std::string code;           // eight digits
    std::string advertised_name;
    std::string notice;         // optional line, e.g. why a new code was drawn
};

#endif  // SETUP_CODE_SCREEN_H_
