#ifndef EIDOLON_SETUP_CODE_SECRET_H_
#define EIDOLON_SETUP_CODE_SECRET_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eidolon {

// The secret behind the code a device shows on its own screen while a setup
// window is open (SDK golden/setup-code-verifier.json, DF-COMM-019).
//
// The session underneath is protocomm Security 2 (SRP-6a). It used to be keyed
// with one development passphrase every build shared and every controller
// hard-coded, which authenticated nothing. A device with a screen now draws an
// eight-digit code per window and derives the SRP verifier from it here; the
// code leaves the device only as pixels.
//
// Deliberately not esp_srp_gen_salt_verifier(): it hashes the salt after its
// leading zero byte has been dropped while still reporting sixteen bytes, and
// Espressif's clients drop that byte too. Either way a salt starting with zero
// fails about one handshake in 256 for a reason neither end can see, so a salt
// starting with zero is drawn again here instead.
struct SetupCodeSecretPorts {
    // Fill `out` with `length` bytes from a source that is random while the
    // radio is running (esp_fill_random), or a fake in host tests.
    void (*random_bytes)(std::uint8_t* out, std::size_t length) = nullptr;
    bool (*sha512)(const std::uint8_t* data, std::size_t length,
                   std::array<std::uint8_t, 64>& digest) = nullptr;
    // out = generator^exponent mod modulus, written as `out_length` big-endian
    // bytes with leading zeros.
    bool (*mod_exp)(std::uint32_t generator,
                    const std::uint8_t* exponent, std::size_t exponent_length,
                    const std::uint8_t* modulus, std::size_t modulus_length,
                    std::uint8_t* out, std::size_t out_length) = nullptr;
};

inline constexpr std::size_t kSetupCodeDigits = 8;
inline constexpr std::size_t kSetupCodeSaltBytes = 16;
inline constexpr std::size_t kSetupCodeVerifierBytes = 384;
inline constexpr std::uint32_t kSrp3072Generator = 5;
inline constexpr const char* kSetupCodeUsername = "eidolon-setup";

// RFC 5054 Appendix A, 3072-bit group, the one protocomm Security 2 uses.
extern const std::array<std::uint8_t, 384> kSrp3072Modulus;

struct SetupCodeSecret {
    std::string code;
    std::array<std::uint8_t, kSetupCodeSaltBytes> salt{};
    std::array<std::uint8_t, kSetupCodeVerifierBytes> verifier{};

    // Overwrite the code and both buffers. Called when a window closes: the
    // code is meant to exist for exactly one window.
    void Clear();
};

// Eight ASCII digits, and not a code a person would try first (all one digit,
// 12345678, 87654321 — the set Matter refuses for the same reason).
bool IsAcceptableSetupCode(std::string_view code);

bool IsAcceptableSetupSalt(const std::array<std::uint8_t, kSetupCodeSaltBytes>& salt);

// x = SHA-512(salt || SHA-512(username ":" password)), v = g^x mod N.
bool DeriveSetupCodeVerifier(
    const SetupCodeSecretPorts& ports,
    std::string_view username,
    std::string_view password,
    const std::array<std::uint8_t, kSetupCodeSaltBytes>& salt,
    std::array<std::uint8_t, kSetupCodeVerifierBytes>& verifier);

// Draw a fresh code and salt and derive the verifier. False if the random
// source kept producing values that had to be refused, or a port failed; the
// caller must then not open the window with anything this left behind.
bool DrawSetupCodeSecret(const SetupCodeSecretPorts& ports, SetupCodeSecret& secret);

// Known-answer check of the ports against a pair produced by ESP-IDF's own
// srp6a.py: the firmware's development salt and verifier for
// eidolon-setup / eidolon-dev-setup. Run before the first window a device
// derives itself, so a broken modular exponentiation is refused rather than
// shown as a code no phone can use.
bool SetupCodeSecretSelfTest(
    const SetupCodeSecretPorts& ports,
    std::string_view known_password,
    std::string_view known_salt_hex,
    std::string_view known_verifier_hex);

// "40718256" -> "4071 8256": how the digits are drawn for a person to type.
std::string FormatSetupCodeForDisplay(std::string_view code);

}  // namespace eidolon

#endif  // EIDOLON_SETUP_CODE_SECRET_H_
