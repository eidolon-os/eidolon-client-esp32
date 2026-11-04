#include "eidolon/setup_code_secret.h"

#include <cstring>
#include <vector>

namespace eidolon {

const std::array<std::uint8_t, 384> kSrp3072Modulus = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2,
    0x21, 0x68, 0xc2, 0x34, 0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1,
    0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74, 0x02, 0x0b, 0xbe, 0xa6,
    0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d,
    0xf2, 0x5f, 0x14, 0x37, 0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45,
    0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6, 0xf4, 0x4c, 0x42, 0xe9,
    0xa6, 0x37, 0xed, 0x6b, 0x0b, 0xff, 0x5c, 0xb6, 0xf4, 0x06, 0xb7, 0xed,
    0xee, 0x38, 0x6b, 0xfb, 0x5a, 0x89, 0x9f, 0xa5, 0xae, 0x9f, 0x24, 0x11,
    0x7c, 0x4b, 0x1f, 0xe6, 0x49, 0x28, 0x66, 0x51, 0xec, 0xe4, 0x5b, 0x3d,
    0xc2, 0x00, 0x7c, 0xb8, 0xa1, 0x63, 0xbf, 0x05, 0x98, 0xda, 0x48, 0x36,
    0x1c, 0x55, 0xd3, 0x9a, 0x69, 0x16, 0x3f, 0xa8, 0xfd, 0x24, 0xcf, 0x5f,
    0x83, 0x65, 0x5d, 0x23, 0xdc, 0xa3, 0xad, 0x96, 0x1c, 0x62, 0xf3, 0x56,
    0x20, 0x85, 0x52, 0xbb, 0x9e, 0xd5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6d,
    0x67, 0x0c, 0x35, 0x4e, 0x4a, 0xbc, 0x98, 0x04, 0xf1, 0x74, 0x6c, 0x08,
    0xca, 0x18, 0x21, 0x7c, 0x32, 0x90, 0x5e, 0x46, 0x2e, 0x36, 0xce, 0x3b,
    0xe3, 0x9e, 0x77, 0x2c, 0x18, 0x0e, 0x86, 0x03, 0x9b, 0x27, 0x83, 0xa2,
    0xec, 0x07, 0xa2, 0x8f, 0xb5, 0xc5, 0x5d, 0xf0, 0x6f, 0x4c, 0x52, 0xc9,
    0xde, 0x2b, 0xcb, 0xf6, 0x95, 0x58, 0x17, 0x18, 0x39, 0x95, 0x49, 0x7c,
    0xea, 0x95, 0x6a, 0xe5, 0x15, 0xd2, 0x26, 0x18, 0x98, 0xfa, 0x05, 0x10,
    0x15, 0x72, 0x8e, 0x5a, 0x8a, 0xaa, 0xc4, 0x2d, 0xad, 0x33, 0x17, 0x0d,
    0x04, 0x50, 0x7a, 0x33, 0xa8, 0x55, 0x21, 0xab, 0xdf, 0x1c, 0xba, 0x64,
    0xec, 0xfb, 0x85, 0x04, 0x58, 0xdb, 0xef, 0x0a, 0x8a, 0xea, 0x71, 0x57,
    0x5d, 0x06, 0x0c, 0x7d, 0xb3, 0x97, 0x0f, 0x85, 0xa6, 0xe1, 0xe4, 0xc7,
    0xab, 0xf5, 0xae, 0x8c, 0xdb, 0x09, 0x33, 0xd7, 0x1e, 0x8c, 0x94, 0xe0,
    0x4a, 0x25, 0x61, 0x9d, 0xce, 0xe3, 0xd2, 0x26, 0x1a, 0xd2, 0xee, 0x6b,
    0xf1, 0x2f, 0xfa, 0x06, 0xd9, 0x8a, 0x08, 0x64, 0xd8, 0x76, 0x02, 0x73,
    0x3e, 0xc8, 0x6a, 0x64, 0x52, 0x1f, 0x2b, 0x18, 0x17, 0x7b, 0x20, 0x0c,
    0xbb, 0xe1, 0x17, 0x57, 0x7a, 0x61, 0x5d, 0x6c, 0x77, 0x09, 0x88, 0xc0,
    0xba, 0xd9, 0x46, 0xe2, 0x08, 0xe2, 0x4f, 0xa0, 0x74, 0xe5, 0xab, 0x31,
    0x43, 0xdb, 0x5b, 0xfc, 0xe0, 0xfd, 0x10, 0x8e, 0x4b, 0x82, 0xd1, 0x20,
    0xa9, 0x3a, 0xd2, 0xca, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

namespace {

// Bounded so a broken random source fails the window instead of spinning. The
// odds of a sound source needing more than a handful of draws are negligible.
constexpr int kMaxDraws = 64;

// The largest multiple of 10^8 that fits in 32 bits. Draws at or above it are
// refused rather than reduced, so every code is equally likely.
constexpr std::uint32_t kUnbiasedCeiling = 4200000000u;

void Wipe(void* data, std::size_t length)
{
    volatile auto* bytes = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = 0;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool DecodeHex(std::string_view hex, std::uint8_t* out, std::size_t length)
{
    if (hex.size() != length * 2) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const int high = HexValue(hex[2 * i]);
        const int low = HexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

}  // namespace

void SetupCodeSecret::Clear()
{
    if (!code.empty()) Wipe(&code[0], code.size());
    code.clear();
    Wipe(salt.data(), salt.size());
    Wipe(verifier.data(), verifier.size());
}

bool IsAcceptableSetupCode(std::string_view code)
{
    if (code.size() != kSetupCodeDigits) return false;
    bool all_same = true;
    for (char c : code) {
        if (c < '0' || c > '9') return false;
        all_same = all_same && c == code[0];
    }
    return !all_same && code != "12345678" && code != "87654321";
}

bool IsAcceptableSetupSalt(const std::array<std::uint8_t, kSetupCodeSaltBytes>& salt)
{
    return salt[0] != 0;
}

bool DeriveSetupCodeVerifier(
    const SetupCodeSecretPorts& ports,
    std::string_view username,
    std::string_view password,
    const std::array<std::uint8_t, kSetupCodeSaltBytes>& salt,
    std::array<std::uint8_t, kSetupCodeVerifierBytes>& verifier)
{
    if (ports.sha512 == nullptr || ports.mod_exp == nullptr) return false;
    std::vector<std::uint8_t> credentials(username.begin(), username.end());
    credentials.push_back(':');
    credentials.insert(credentials.end(), password.begin(), password.end());
    std::array<std::uint8_t, 64> inner{};
    const bool hashed = ports.sha512(credentials.data(), credentials.size(), inner);
    Wipe(credentials.data(), credentials.size());
    if (!hashed) return false;

    std::array<std::uint8_t, kSetupCodeSaltBytes + 64> salted{};
    std::memcpy(salted.data(), salt.data(), salt.size());
    std::memcpy(salted.data() + salt.size(), inner.data(), inner.size());
    std::array<std::uint8_t, 64> x{};
    const bool derived = ports.sha512(salted.data(), salted.size(), x);
    Wipe(inner.data(), inner.size());
    Wipe(salted.data(), salted.size());
    if (!derived) return false;

    const bool exponentiated = ports.mod_exp(
        kSrp3072Generator, x.data(), x.size(), kSrp3072Modulus.data(), kSrp3072Modulus.size(),
        verifier.data(), verifier.size());
    Wipe(x.data(), x.size());
    return exponentiated;
}

bool DrawSetupCodeSecret(const SetupCodeSecretPorts& ports, SetupCodeSecret& secret)
{
    secret.Clear();
    if (ports.random_bytes == nullptr) return false;

    bool have_code = false;
    for (int draw = 0; draw < kMaxDraws && !have_code; ++draw) {
        std::uint8_t raw[4] = {};
        ports.random_bytes(raw, sizeof(raw));
        const std::uint32_t value = (static_cast<std::uint32_t>(raw[0]) << 24) |
                                    (static_cast<std::uint32_t>(raw[1]) << 16) |
                                    (static_cast<std::uint32_t>(raw[2]) << 8) |
                                    static_cast<std::uint32_t>(raw[3]);
        Wipe(raw, sizeof(raw));
        if (value >= kUnbiasedCeiling) continue;
        char digits[kSetupCodeDigits + 1] = {};
        std::uint32_t remaining = value % 100000000u;
        for (int i = static_cast<int>(kSetupCodeDigits) - 1; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + remaining % 10);
            remaining /= 10;
        }
        if (IsAcceptableSetupCode(std::string_view(digits, kSetupCodeDigits))) {
            secret.code.assign(digits, kSetupCodeDigits);
            have_code = true;
        }
        Wipe(digits, sizeof(digits));
    }

    bool have_salt = false;
    for (int draw = 0; draw < kMaxDraws && have_code && !have_salt; ++draw) {
        ports.random_bytes(secret.salt.data(), secret.salt.size());
        have_salt = IsAcceptableSetupSalt(secret.salt);
    }

    if (!have_code || !have_salt ||
        !DeriveSetupCodeVerifier(ports, kSetupCodeUsername, secret.code, secret.salt,
                                 secret.verifier)) {
        secret.Clear();
        return false;
    }
    return true;
}

bool SetupCodeSecretSelfTest(
    const SetupCodeSecretPorts& ports,
    std::string_view known_password,
    std::string_view known_salt_hex,
    std::string_view known_verifier_hex)
{
    std::array<std::uint8_t, kSetupCodeSaltBytes> salt{};
    std::array<std::uint8_t, kSetupCodeVerifierBytes> expected{};
    if (!DecodeHex(known_salt_hex, salt.data(), salt.size()) ||
        !DecodeHex(known_verifier_hex, expected.data(), expected.size())) {
        return false;
    }
    std::array<std::uint8_t, kSetupCodeVerifierBytes> actual{};
    if (!DeriveSetupCodeVerifier(ports, kSetupCodeUsername, known_password, salt, actual)) {
        return false;
    }
    return actual == expected;
}

std::string FormatSetupCodeForDisplay(std::string_view code)
{
    if (code.size() != kSetupCodeDigits) return std::string(code);
    std::string spaced(code.substr(0, 4));
    spaced.push_back(' ');
    spaced.append(code.substr(4));
    return spaced;
}

}  // namespace eidolon
