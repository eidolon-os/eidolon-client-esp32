#include "eidolon/rfc3339_utc.h"

#include <cassert>
#include <cstdint>

using eidolon::IsRfc3339DeadlineExpired;
using eidolon::ParseRfc3339UtcMillis;

namespace {

int64_t Parse(const char* value) {
    int64_t result = -1;
    assert(ParseRfc3339UtcMillis(value, result));
    return result;
}

void EpochAndOffsetsAreTimezoneIndependent() {
    assert(Parse("1970-01-01T00:00:00Z") == 0);
    assert(Parse("2026-08-30T08:00:00+08:00") ==
           Parse("2026-08-30T00:00:00Z"));
    assert(Parse("2026-08-29T18:30:00-05:30") ==
           Parse("2026-08-30T00:00:00Z"));
    assert(Parse("2026-08-30t00:00:00.123456z") ==
           Parse("2026-08-30T00:00:00.123Z"));
}

void LeapYearAndCenturyRulesAreCorrect() {
    assert(Parse("2024-02-29T00:00:00Z") -
               Parse("2024-02-28T00:00:00Z") ==
           86400000);
    int64_t ignored = 0;
    assert(!ParseRfc3339UtcMillis("2023-02-29T00:00:00Z", ignored));
    assert(!ParseRfc3339UtcMillis("2100-02-29T00:00:00Z", ignored));
    assert(ParseRfc3339UtcMillis("2000-02-29T00:00:00Z", ignored));
}

void InvalidDatesZonesAndTrailingDataFailClosed() {
    const char* invalid[] = {
        "2026-04-31T00:00:00Z",
        "2026-08-30T24:00:00Z",
        "2026-08-30T00:60:00Z",
        "2026-08-30T00:00:00",
        "2026-08-30T00:00:00+24:00",
        "2026-08-30T00:00:00+08:60",
        "2026-08-30T00:00:00.Z",
        "2026-08-30T00:00:00Zjunk",
    };
    for (const char* value : invalid) {
        int64_t ignored = 0;
        assert(!ParseRfc3339UtcMillis(value, ignored));
    }
}

void DeadlineComparisonAndUntrustedClockFailClosed() {
    const int64_t due = Parse("2026-08-30T00:00:00.500Z");
    assert(!IsRfc3339DeadlineExpired(
        "2026-08-30T00:00:00.500Z", due - 1, 0));
    assert(IsRfc3339DeadlineExpired(
        "2026-08-30T00:00:00.500Z", due, 0));
    assert(IsRfc3339DeadlineExpired("invalid", due - 1, 0));
    assert(IsRfc3339DeadlineExpired(
        "2026-08-30T00:00:00.500Z", 1000, 2000));
}

}  // namespace

int main() {
    EpochAndOffsetsAreTimezoneIndependent();
    LeapYearAndCenturyRulesAreCorrect();
    InvalidDatesZonesAndTrailingDataFailClosed();
    DeadlineComparisonAndUntrustedClockFailClosed();
    return 0;
}
