#ifndef EIDOLON_RFC3339_UTC_H_
#define EIDOLON_RFC3339_UTC_H_

#include <cstdint>
#include <string>

namespace eidolon {

// Parses RFC3339 without consulting process-local timezone state. Additional
// fractional digits are truncated to milliseconds, which can only fail closed.
bool ParseRfc3339UtcMillis(const std::string& value, int64_t& utc_millis);

// Invalid timestamps and untrusted wall clocks are expired by definition.
bool IsRfc3339DeadlineExpired(const std::string& deadline,
                              int64_t now_utc_millis,
                              int64_t minimum_trusted_utc_millis);

}  // namespace eidolon

#endif  // EIDOLON_RFC3339_UTC_H_
