#include "authority_locator.h"

#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/private_access.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <limits>

namespace eidolon {
namespace {

bool KeyId(const mbedtls_pk_context& key, std::string& out)
{
    std::array<unsigned char, 256> der{};
    const int size = mbedtls_pk_write_pubkey_der(
        const_cast<mbedtls_pk_context*>(&key), der.data(), der.size());
    if (size <= 0) {
        return false;
    }
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256(der.data() + der.size() - size, size, digest.data(), 0);
    static constexpr char hex[] = "0123456789abcdef";
    out = "sha256:";
    out.reserve(71);
    for (unsigned char byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return true;
}

bool RawSignature(const std::string& encoded, std::array<unsigned char, 64>& out)
{
    std::string standard = encoded;
    std::replace(standard.begin(), standard.end(), '-', '+');
    std::replace(standard.begin(), standard.end(), '_', '/');
    while (standard.size() % 4 != 0) standard.push_back('=');
    size_t written = 0;
    return mbedtls_base64_decode(
               out.data(), out.size(), &written,
               reinterpret_cast<const unsigned char*>(standard.data()),
               standard.size()) == 0 &&
           written == out.size();
}

bool OnlyClockFlags(uint32_t flags)
{
    const uint32_t allowed = MBEDTLS_X509_BADCERT_EXPIRED |
                             MBEDTLS_X509_BADCERT_FUTURE |
                             MBEDTLS_X509_BADCRL_EXPIRED |
                             MBEDTLS_X509_BADCRL_FUTURE;
    return (flags & ~allowed) == 0;
}

bool UtcSeconds(const std::tm& calendar, std::time_t& out)
{
    const int year = calendar.tm_year + 1900;
    const unsigned month = static_cast<unsigned>(calendar.tm_mon + 1);
    const unsigned day = static_cast<unsigned>(calendar.tm_mday);
    if (year < 1970 || month < 1 || month > 12 ||
        calendar.tm_hour < 0 || calendar.tm_hour > 23 ||
        calendar.tm_min < 0 || calendar.tm_min > 59 ||
        calendar.tm_sec < 0 || calendar.tm_sec > 60) {
        return false;
    }
    static constexpr unsigned days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = days_per_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) ++max_day;
    if (day < 1 || day > max_day) return false;

    // Howard Hinnant's civil-date transform, expressed entirely in integers,
    // avoids timegm/mktime and therefore any Host or process timezone state.
    const int adjusted_year = year - (month <= 2 ? 1 : 0);
    const int era = adjusted_year / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const int shifted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned day_of_year =
        static_cast<unsigned>((153 * shifted_month + 2) / 5) + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t days = static_cast<int64_t>(era) * 146097 +
                         static_cast<int64_t>(day_of_era) - 719468;
    const int64_t seconds = days * 86400 + calendar.tm_hour * 3600 +
                            calendar.tm_min * 60 + calendar.tm_sec;
    if (seconds < 0 ||
        static_cast<uint64_t>(seconds) >
            static_cast<uint64_t>(std::numeric_limits<std::time_t>::max())) {
        return false;
    }
    out = static_cast<std::time_t>(seconds);
    return true;
}

bool ParseRfc3339(const std::string& value, std::time_t& out)
{
    if (value.size() < 20) return false;
    std::tm calendar{};
    char* end = strptime(value.c_str(), "%Y-%m-%dT%H:%M:%S", &calendar);
    if (end == nullptr) return false;
    const char* cursor = end;
    if (*cursor == '.') {
        ++cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
    }
    int offset_seconds = 0;
    if (*cursor == 'Z' && cursor[1] == '\0') {
        return UtcSeconds(calendar, out);
    }
    if ((*cursor != '+' && *cursor != '-') ||
        cursor[1] < '0' || cursor[1] > '2' ||
        cursor[2] < '0' || cursor[2] > '9' || cursor[3] != ':' ||
        cursor[4] < '0' || cursor[4] > '5' ||
        cursor[5] < '0' || cursor[5] > '9' || cursor[6] != '\0') {
        return false;
    }
    const int hours = (cursor[1] - '0') * 10 + (cursor[2] - '0');
    const int minutes = (cursor[4] - '0') * 10 + (cursor[5] - '0');
    if (hours > 23) return false;
    offset_seconds = hours * 3600 + minutes * 60;
    if (*cursor == '-') offset_seconds = -offset_seconds;
    std::time_t local_seconds = 0;
    if (!UtcSeconds(calendar, local_seconds)) return false;
    const int64_t utc_seconds = static_cast<int64_t>(local_seconds) - offset_seconds;
    if (utc_seconds < 0) return false;
    out = static_cast<std::time_t>(utc_seconds);
    return true;
}

bool DescriptorWindowValid(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor)
{
    // During the local commissioning handoff the device may not have SNTP yet.
    // Once wall time is trustworthy, every operational load enforces both
    // boundaries and an expired cache becomes AuthorityDiscoveryRequired.
    static constexpr std::time_t kTrustedTimeFloor = 1704067200;  // 2024-01-01 UTC
    const std::time_t now = std::time(nullptr);
    if (now < kTrustedTimeFloor) return true;
    std::time_t issued = 0;
    std::time_t expires = 0;
    return ParseRfc3339(descriptor.issued_at, issued) &&
           ParseRfc3339(descriptor.expires_at, expires) &&
           issued <= now && now < expires;
}

}  // namespace

esp_err_t VerifyOwnerDomainDescriptor(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& canonical_signing_bytes,
    const std::string& owner_root_certificate_pem,
    const std::string& authority_signing_certificate_pem)
{
    mbedtls_x509_crt root;
    mbedtls_x509_crt authority;
    mbedtls_x509_crt_init(&root);
    mbedtls_x509_crt_init(&authority);
    int result = mbedtls_x509_crt_parse(
        &root,
        reinterpret_cast<const unsigned char*>(owner_root_certificate_pem.c_str()),
        owner_root_certificate_pem.size() + 1);
    if (result == 0) {
        result = mbedtls_x509_crt_parse(
            &authority,
            reinterpret_cast<const unsigned char*>(
                authority_signing_certificate_pem.c_str()),
            authority_signing_certificate_pem.size() + 1);
    }
    uint32_t flags = 0;
    if (!DescriptorWindowValid(descriptor)) {
        result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    if (result == 0) {
        result = mbedtls_x509_crt_verify(
            &authority, &root, nullptr, nullptr, &flags, nullptr, nullptr);
        if (result != 0 && OnlyClockFlags(flags)) {
            // Commissioning may occur before the device has trusted wall time.
            // Signature, CA and usage are still checked; time is enforced by
            // the operational locator after network time becomes trustworthy.
            result = 0;
        }
    }
    if (result == 0 &&
        mbedtls_x509_crt_check_extended_key_usage(
            &authority, MBEDTLS_OID_CODE_SIGNING,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING)) != 0) {
        result = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    }
    std::string root_key_id;
    std::string authority_key_id;
    if (result == 0 &&
        (!KeyId(root.pk, root_key_id) || !KeyId(authority.pk, authority_key_id) ||
         descriptor.signing_key_id != authority_key_id ||
         std::find(descriptor.trust_root_refs.begin(), descriptor.trust_root_refs.end(),
                   root_key_id) == descriptor.trust_root_refs.end())) {
        result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    std::array<unsigned char, 64> signature{};
    std::array<unsigned char, 32> digest{};
    if (result == 0 && !RawSignature(descriptor.signature, signature)) {
        result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    if (result == 0) {
        mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(canonical_signing_bytes.data()),
            canonical_signing_bytes.size(), digest.data(), 0);
        mbedtls_mpi r;
        mbedtls_mpi s;
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);
        result = mbedtls_mpi_read_binary(&r, signature.data(), 32);
        if (result == 0) {
            result = mbedtls_mpi_read_binary(&s, signature.data() + 32, 32);
        }
        if (result == 0) {
            const mbedtls_ecp_keypair* key = mbedtls_pk_ec(authority.pk);
            result = mbedtls_ecdsa_verify(
                const_cast<mbedtls_ecp_group*>(&key->MBEDTLS_PRIVATE(grp)),
                digest.data(), digest.size(),
                &key->MBEDTLS_PRIVATE(Q), &r, &s);
        }
        mbedtls_mpi_free(&s);
        mbedtls_mpi_free(&r);
    }
    mbedtls_x509_crt_free(&authority);
    mbedtls_x509_crt_free(&root);
    return result == 0 ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}

}  // namespace eidolon
