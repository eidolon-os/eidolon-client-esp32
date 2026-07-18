#pragma once

#include <string>

namespace eidolon {

inline std::string GuardJsonEscape(const std::string& value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                escaped += "\\u00";
                escaped += kHex[(ch >> 4) & 0x0f];
                escaped += kHex[ch & 0x0f];
            } else {
                escaped += static_cast<char>(ch);
            }
        }
    }
    return escaped;
}

}  // namespace eidolon
