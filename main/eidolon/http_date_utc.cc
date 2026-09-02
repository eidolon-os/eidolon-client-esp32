#include "http_date_utc.h"

#include "rfc3339_utc.h"

#include <cstddef>
#include <cstdio>

namespace eidolon {
namespace {

// "Sun, 06 Nov 1994 08:49:37 GMT" — fixed length, fixed field positions.
constexpr size_t kImfFixdateLength = 29;

bool Digits(const std::string& value, size_t offset, size_t count) {
    for (size_t index = offset; index < offset + count; ++index) {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

bool DayNameAt(const std::string& value, size_t offset) {
    static constexpr const char* names[] = {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (const char* name : names) {
        if (value.compare(offset, 3, name) == 0) return true;
    }
    return false;
}

// One-based month, or zero for a name no server is allowed to send.
int MonthAt(const std::string& value, size_t offset) {
    static constexpr const char* names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int index = 0; index < 12; ++index) {
        if (value.compare(offset, 3, names[index]) == 0) return index + 1;
    }
    return 0;
}

}  // namespace

bool ParseHttpDateUtcMillis(const std::string& value, int64_t& utc_millis) {
    if (value.size() != kImfFixdateLength || !DayNameAt(value, 0) ||
        value[3] != ',' || value[4] != ' ' || value[7] != ' ' ||
        value[11] != ' ' || value[16] != ' ' || value[19] != ':' ||
        value[22] != ':' || value[25] != ' ' ||
        value.compare(26, 3, "GMT") != 0 || !Digits(value, 5, 2) ||
        !Digits(value, 12, 4) || !Digits(value, 17, 2) ||
        !Digits(value, 20, 2) || !Digits(value, 23, 2)) {
        return false;
    }
    const int month = MonthAt(value, 8);
    if (month == 0) return false;

    // The same instant in the syntax this firmware already has one parser for.
    // Month lengths, the leap-year rules and the civil-days conversion are
    // decided there, and a second implementation of them here would be a second
    // answer to a question that has one.
    char rfc3339[21];
    std::snprintf(rfc3339, sizeof(rfc3339), "%.4s-%02d-%.2sT%.2s:%.2s:%.2sZ",
                  value.c_str() + 12, month, value.c_str() + 5,
                  value.c_str() + 17, value.c_str() + 20,
                  value.c_str() + 23);
    return ParseRfc3339UtcMillis(rfc3339, utc_millis);
}

}  // namespace eidolon
