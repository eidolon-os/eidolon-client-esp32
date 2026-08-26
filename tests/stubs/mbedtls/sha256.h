#ifndef TEST_STUB_MBEDTLS_SHA256_H_
#define TEST_STUB_MBEDTLS_SHA256_H_

#include <cstddef>
#include <cstdint>

inline int mbedtls_sha256(
    const unsigned char* input,
    std::size_t length,
    unsigned char output[32],
    int)
{
    std::uint32_t state = 2166136261u;
    for (std::size_t index = 0; index < length; ++index) {
        state = (state ^ input[index]) * 16777619u;
    }
    for (std::size_t index = 0; index < 32; ++index) {
        state = (state ^ static_cast<std::uint32_t>(index)) * 16777619u;
        output[index] = static_cast<unsigned char>(state >> ((index % 4) * 8));
    }
    return 0;
}

#endif
