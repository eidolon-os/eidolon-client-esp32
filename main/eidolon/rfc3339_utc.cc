#include "rfc3339_utc.h"

#include <cstddef>

namespace eidolon {
namespace {

bool Digit(char value) {
    return value >= '0' && value <= '9';
}

bool Number(const std::string& value, size_t offset, size_t count, int& out) {
    if (offset + count > value.size()) return false;
    int result = 0;
    for (size_t index = offset; index < offset + count; ++index) {
        if (!Digit(value[index])) return false;
        result = result * 10 + value[index] - '0';
    }
    out = result;
    return true;
}

bool LeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) {
    static constexpr int days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && LeapYear(year)) return 29;
    return days[month - 1];
}

// Howard Hinnant's proleptic-Gregorian civil-date conversion. The result is
// days since 1970-01-01 and is independent of libc timezone configuration.
int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 +
           static_cast<int64_t>(day_of_era) - 719468;
}

}  // namespace

bool ParseRfc3339UtcMillis(const std::string& value, int64_t& utc_millis) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' ||
        value[16] != ':' || !Number(value, 0, 4, year) ||
        !Number(value, 5, 2, month) || !Number(value, 8, 2, day) ||
        !Number(value, 11, 2, hour) || !Number(value, 14, 2, minute) ||
        !Number(value, 17, 2, second) || year < 1 || year > 9999 ||
        month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour > 23 || minute > 59 ||
        second > 60) {
        return false;
    }

    size_t position = 19;
    int fraction_millis = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const size_t fraction_start = position;
        int digits = 0;
        while (position < value.size() && Digit(value[position])) {
            if (digits < 3) {
                fraction_millis =
                    fraction_millis * 10 + value[position] - '0';
            }
            ++digits;
            ++position;
        }
        if (position == fraction_start) return false;
        while (digits < 3) {
            fraction_millis *= 10;
            ++digits;
        }
    }

    int offset_seconds = 0;
    if (position < value.size() &&
        (value[position] == 'Z' || value[position] == 'z')) {
        ++position;
    } else if (position < value.size() &&
               (value[position] == '+' || value[position] == '-')) {
        const bool positive = value[position] == '+';
        int offset_hour = 0;
        int offset_minute = 0;
        if (position + 6 != value.size() || value[position + 3] != ':' ||
            !Number(value, position + 1, 2, offset_hour) ||
            !Number(value, position + 4, 2, offset_minute) ||
            offset_hour > 23 || offset_minute > 59) {
            return false;
        }
        offset_seconds = (offset_hour * 60 + offset_minute) * 60;
        if (!positive) offset_seconds = -offset_seconds;
        position += 6;
    } else {
        return false;
    }
    if (position != value.size()) return false;

    const int64_t days = DaysFromCivil(
        year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t seconds =
        days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    utc_millis = seconds * 1000 + fraction_millis;
    return true;
}

Rfc3339DeadlineState EvaluateRfc3339Deadline(
    const std::string& deadline,
    int64_t now_utc_millis,
    int64_t minimum_trusted_utc_millis) {
    int64_t due_utc_millis = 0;
    if (now_utc_millis < minimum_trusted_utc_millis ||
        !ParseRfc3339UtcMillis(deadline, due_utc_millis)) {
        return Rfc3339DeadlineState::Unknown;
    }
    return now_utc_millis >= due_utc_millis ? Rfc3339DeadlineState::Expired
                                            : Rfc3339DeadlineState::Live;
}

}  // namespace eidolon
