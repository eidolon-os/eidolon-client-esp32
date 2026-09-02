#ifndef EIDOLON_HTTP_DATE_UTC_H_
#define EIDOLON_HTTP_DATE_UTC_H_

#include <cstdint>
#include <string>

namespace eidolon {

// Reads the `Date` header of an HTTP response into UTC milliseconds.
//
// Only the IMF-fixdate form RFC 9110 requires a server to send is accepted
// ("Sun, 06 Nov 1994 08:49:37 GMT"); the two obsolete forms are refused rather
// than guessed at. False means "this response did not state a time this device
// can read", which is not the same as a time of zero, and callers that use the
// result to judge a deadline must keep those apart.
bool ParseHttpDateUtcMillis(const std::string& value, int64_t& utc_millis);

}  // namespace eidolon

#endif  // EIDOLON_HTTP_DATE_UTC_H_
