#include "eidolon/http_date_utc.h"

#include "eidolon/rfc3339_utc.h"

#include <cassert>
#include <cstdint>

using eidolon::ParseHttpDateUtcMillis;
using eidolon::ParseRfc3339UtcMillis;

namespace {

int64_t Http(const char* value) {
    int64_t result = -1;
    assert(ParseHttpDateUtcMillis(value, result));
    return result;
}

int64_t Rfc3339(const char* value) {
    int64_t result = -1;
    assert(ParseRfc3339UtcMillis(value, result));
    return result;
}

void AServerDateIsTheSameInstantAsItsRfc3339Spelling() {
    assert(Http("Sun, 06 Nov 1994 08:49:37 GMT") ==
           Rfc3339("1994-11-06T08:49:37Z"));
    assert(Http("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    assert(Http("Wed, 02 Sep 2026 09:04:32 GMT") ==
           Rfc3339("2026-09-02T09:04:32Z"));
    // Every month name resolves, so a device does not stop being able to read
    // the time for four weeks of the year.
    assert(Http("Sun, 01 Feb 2026 00:00:00 GMT") ==
           Rfc3339("2026-02-01T00:00:00Z"));
    assert(Http("Tue, 01 Dec 2026 00:00:00 GMT") ==
           Rfc3339("2026-12-01T00:00:00Z"));
}

void MalformedAndObsoleteDatesAreRefusedRatherThanGuessedAt() {
    const char* refused[] = {
        // The two obsolete HTTP-date forms. A server is not allowed to send
        // them and this device does not have to invent what they mean.
        "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994",
        // Not GMT, so the stated instant is not the one it looks like.
        "Sun, 06 Nov 1994 08:49:37 +0100",
        "Sun, 06 Nov 1994 08:49:37 UTC",
        "Nov, 06 Nov 1994 08:49:37 GMT",
        "Sun, 06 Nov 1994 08:49:37 GMT ",
        "Sun, 06 Nov 1994 08:49:37 GM",
        "Sun; 06 Nov 1994 08:49:37 GMT",
        "Sun, 06 Foo 1994 08:49:37 GMT",
        "Sun, 6 Nov 1994 08:49:37 GMTX",
        "Sun, 06 Nov 199A 08:49:37 GMT",
        // Refused by the one parser that owns the calendar rules.
        "Mon, 29 Feb 2023 08:49:37 GMT",
        "Mon, 31 Apr 2026 08:49:37 GMT",
        "Mon, 06 Nov 1994 24:49:37 GMT",
        "",
    };
    for (const char* value : refused) {
        int64_t ignored = -1;
        assert(!ParseHttpDateUtcMillis(value, ignored));
        assert(ignored == -1);
    }
}

}  // namespace

int main() {
    AServerDateIsTheSameInstantAsItsRfc3339Spelling();
    MalformedAndObsoleteDatesAreRefusedRatherThanGuessedAt();
    return 0;
}
