#ifndef EIDOLON_RFC3339_UTC_H_
#define EIDOLON_RFC3339_UTC_H_

#include <cstdint>
#include <string>

namespace eidolon {

// Parses RFC3339 without consulting process-local timezone state. Additional
// fractional digits are truncated to milliseconds, which can only fail closed.
bool ParseRfc3339UtcMillis(const std::string& value, int64_t& utc_millis);

enum class Rfc3339DeadlineState {
    // The deadline could not be read, or this device's wall clock cannot be
    // trusted to compare against it. Not the same as expired: it is the absence
    // of an answer, and only the caller knows whether that is safe to act on.
    Unknown,
    Live,
    Expired,
};

Rfc3339DeadlineState EvaluateRfc3339Deadline(
    const std::string& deadline,
    int64_t now_utc_millis,
    int64_t minimum_trusted_utc_millis);

}  // namespace eidolon

#endif  // EIDOLON_RFC3339_UTC_H_
