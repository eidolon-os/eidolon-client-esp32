#ifndef EIDOLON_ESP_IDF_SETUP_CODE_SECRET_H_
#define EIDOLON_ESP_IDF_SETUP_CODE_SECRET_H_

#include "eidolon/setup_code_secret.h"

namespace eidolon {

// The device's ports for the setup code secret: esp_fill_random, and mbedtls'
// SHA-512 and modular exponentiation (hardware MPI where the chip has one).
// esp_fill_random is only truly random while Wi-Fi or Bluetooth is running, so
// a caller draws a secret after it has started the radio, never before.
const SetupCodeSecretPorts& EspIdfSetupCodeSecretPorts();

}  // namespace eidolon

#endif  // EIDOLON_ESP_IDF_SETUP_CODE_SECRET_H_
