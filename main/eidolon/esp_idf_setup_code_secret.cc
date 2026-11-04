#include "eidolon/esp_idf_setup_code_secret.h"

// mbedtls 3 (ESP-IDF 5) declares these publicly; mbedtls 4 (ESP-IDF 6) moved
// them under mbedtls/private. The SHA-256 compat header decides which once.
#include "mbedtls_sha256_compat.h"
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
#  include <mbedtls/bignum.h>
#  include <mbedtls/sha512.h>
#else
#  include <mbedtls/private/bignum.h>
#  include <mbedtls/private/sha512.h>
#endif

#include <esp_random.h>

namespace eidolon {
namespace {

void RandomBytes(std::uint8_t* out, std::size_t length)
{
    esp_fill_random(out, length);
}

bool Sha512(const std::uint8_t* data, std::size_t length, std::array<std::uint8_t, 64>& digest)
{
    return mbedtls_sha512(data, length, digest.data(), 0) == 0;
}

bool ModExp(std::uint32_t generator, const std::uint8_t* exponent, std::size_t exponent_length,
            const std::uint8_t* modulus, std::size_t modulus_length, std::uint8_t* out,
            std::size_t out_length)
{
    mbedtls_mpi g;
    mbedtls_mpi x;
    mbedtls_mpi n;
    mbedtls_mpi v;
    mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&v);
    const bool ok =
        mbedtls_mpi_lset(&g, static_cast<mbedtls_mpi_sint>(generator)) == 0 &&
        mbedtls_mpi_read_binary(&x, exponent, exponent_length) == 0 &&
        mbedtls_mpi_read_binary(&n, modulus, modulus_length) == 0 &&
        mbedtls_mpi_exp_mod(&v, &g, &x, &n, nullptr) == 0 &&
        // Writes big-endian, left-padded with zeros to exactly out_length.
        mbedtls_mpi_write_binary(&v, out, out_length) == 0;
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&g);
    return ok;
}

}  // namespace

const SetupCodeSecretPorts& EspIdfSetupCodeSecretPorts()
{
    static const SetupCodeSecretPorts ports = [] {
        SetupCodeSecretPorts value;
        value.random_bytes = RandomBytes;
        value.sha512 = Sha512;
        value.mod_exp = ModExp;
        return value;
    }();
    return ports;
}

}  // namespace eidolon
