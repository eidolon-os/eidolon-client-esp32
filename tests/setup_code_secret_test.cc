#include <cJSON.h>

#include <openssl/bn.h>
#include <openssl/sha.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eidolon/setup_code_secret.h"

namespace {

using eidolon::DeriveSetupCodeVerifier;
using eidolon::DrawSetupCodeSecret;
using eidolon::FormatSetupCodeForDisplay;
using eidolon::IsAcceptableSetupCode;
using eidolon::kSetupCodeSaltBytes;
using eidolon::kSetupCodeVerifierBytes;
using eidolon::kSrp3072Modulus;
using eidolon::SetupCodeSecret;
using eidolon::SetupCodeSecretPorts;
using eidolon::SetupCodeSecretSelfTest;

bool Sha512(const std::uint8_t* data, std::size_t length, std::array<std::uint8_t, 64>& digest)
{
    SHA512(data, length, digest.data());
    return true;
}

bool ModExp(std::uint32_t generator, const std::uint8_t* exponent, std::size_t exponent_length,
            const std::uint8_t* modulus, std::size_t modulus_length, std::uint8_t* out,
            std::size_t out_length)
{
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* g = BN_new();
    BIGNUM* x = BN_bin2bn(exponent, static_cast<int>(exponent_length), nullptr);
    BIGNUM* n = BN_bin2bn(modulus, static_cast<int>(modulus_length), nullptr);
    BIGNUM* v = BN_new();
    const bool ok = ctx && g && x && n && v && BN_set_word(g, generator) &&
                    BN_mod_exp(v, g, x, n, ctx) &&
                    BN_bn2binpad(v, out, static_cast<int>(out_length)) == static_cast<int>(out_length);
    BN_free(v);
    BN_free(n);
    BN_free(x);
    BN_free(g);
    BN_CTX_free(ctx);
    return ok;
}

// A scripted random source, so rejection sampling can be driven on purpose.
std::vector<std::uint8_t> g_script;
std::size_t g_cursor = 0;

void ScriptedRandom(std::uint8_t* out, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = g_cursor < g_script.size() ? g_script[g_cursor] : 0x11;
        ++g_cursor;
    }
}

SetupCodeSecretPorts Ports()
{
    SetupCodeSecretPorts ports;
    ports.random_bytes = ScriptedRandom;
    ports.sha512 = Sha512;
    ports.mod_exp = ModExp;
    return ports;
}

std::string Hex(const std::uint8_t* data, std::size_t length)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

std::string ReadVector()
{
    std::ifstream file("tests/fixtures/device_foundation_v1/setup-code-verifier.json");
    assert(file.good());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// Every vector the SDK pins, through the same derivation the device runs. The
// first is the firmware's own development pair from ESP-IDF's srp6a.py, so a
// pass here means the formula is the one Espressif's clients compute.
void TheVerifierIsTheOneTheGoldenVectorPins()
{
    const std::string text = ReadVector();
    cJSON* root = cJSON_Parse(text.c_str());
    assert(root != nullptr);
    const cJSON* n_hex = cJSON_GetObjectItemCaseSensitive(root, "n_hex");
    assert(Hex(kSrp3072Modulus.data(), kSrp3072Modulus.size()) == n_hex->valuestring);
    const cJSON* vectors = cJSON_GetObjectItemCaseSensitive(root, "vectors");
    int seen = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, vectors) {
        const char* username = cJSON_GetObjectItemCaseSensitive(item, "username")->valuestring;
        const char* password = cJSON_GetObjectItemCaseSensitive(item, "password")->valuestring;
        const char* salt_hex = cJSON_GetObjectItemCaseSensitive(item, "salt_hex")->valuestring;
        const char* verifier_hex = cJSON_GetObjectItemCaseSensitive(item, "verifier_hex")->valuestring;
        std::array<std::uint8_t, kSetupCodeSaltBytes> salt{};
        for (std::size_t i = 0; i < salt.size(); ++i) {
            salt[i] = static_cast<std::uint8_t>(std::stoul(std::string(salt_hex + 2 * i, 2), nullptr, 16));
        }
        std::array<std::uint8_t, kSetupCodeVerifierBytes> verifier{};
        assert(DeriveSetupCodeVerifier(Ports(), username, password, salt, verifier));
        assert(Hex(verifier.data(), verifier.size()) == verifier_hex);
        ++seen;
    }
    assert(seen == 2);

    const cJSON* first = cJSON_GetArrayItem(vectors, 0);
    assert(SetupCodeSecretSelfTest(
        Ports(), cJSON_GetObjectItemCaseSensitive(first, "password")->valuestring,
        cJSON_GetObjectItemCaseSensitive(first, "salt_hex")->valuestring,
        cJSON_GetObjectItemCaseSensitive(first, "verifier_hex")->valuestring));
    assert(!SetupCodeSecretSelfTest(
        Ports(), "eidolon-dev-setuq",
        cJSON_GetObjectItemCaseSensitive(first, "salt_hex")->valuestring,
        cJSON_GetObjectItemCaseSensitive(first, "verifier_hex")->valuestring));
    cJSON_Delete(root);
}

void ACodeAnybodyWouldTryFirstIsNotACode()
{
    assert(IsAcceptableSetupCode("40718256"));
    assert(!IsAcceptableSetupCode("00000000"));
    assert(!IsAcceptableSetupCode("99999999"));
    assert(!IsAcceptableSetupCode("12345678"));
    assert(!IsAcceptableSetupCode("87654321"));
    assert(!IsAcceptableSetupCode("4071825"));
    assert(!IsAcceptableSetupCode("4071825a"));
}

// 0xffffffff is refused (no modulo bias); 0x00bc614e = 12345678 is refused as
// weak; 0x0026d4d0 = 2544848 is drawn as "02544848". The first salt starts with
// zero and is drawn again.
void DrawsAreRefusedUntilTheyAreFairAndSound()
{
    g_script = {0xff, 0xff, 0xff, 0xff, 0x00, 0xbc, 0x61, 0x4e, 0x00, 0x26, 0xd4, 0xd0};
    for (int i = 0; i < 16; ++i) g_script.push_back(i == 0 ? 0x00 : 0x55);
    for (int i = 0; i < 16; ++i) g_script.push_back(static_cast<std::uint8_t>(i + 1));
    g_cursor = 0;
    SetupCodeSecret secret;
    assert(DrawSetupCodeSecret(Ports(), secret));
    assert(secret.code == "02544848");
    assert(secret.salt[0] == 0x01 && secret.salt[15] == 0x10);
    std::array<std::uint8_t, kSetupCodeVerifierBytes> expected{};
    assert(DeriveSetupCodeVerifier(Ports(), "eidolon-setup", "02544848", secret.salt, expected));
    assert(expected == secret.verifier);

    secret.Clear();
    assert(secret.code.empty());
    for (std::uint8_t byte : secret.verifier) assert(byte == 0);
}

void ARandomSourceThatNeverYieldsAFairDrawFailsTheWindow()
{
    g_script.assign(64 * 4, 0xff);
    g_cursor = 0;
    SetupCodeSecret secret;
    assert(!DrawSetupCodeSecret(Ports(), secret));
    assert(secret.code.empty());
}

void TheCodeIsDrawnInTwoGroupsOfFour()
{
    assert(FormatSetupCodeForDisplay("40718256") == "4071 8256");
}

}  // namespace

int main()
{
    TheVerifierIsTheOneTheGoldenVectorPins();
    ACodeAnybodyWouldTryFirstIsNotACode();
    DrawsAreRefusedUntilTheyAreFairAndSound();
    ARandomSourceThatNeverYieldsAFairDrawFailsTheWindow();
    TheCodeIsDrawnInTwoGroupsOfFour();
    return 0;
}
