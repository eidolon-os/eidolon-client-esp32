#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace eidolon {
// Matches the SDK's one active stream per topic. Keep only a bounded subtitle
// prefix; stream chunks may split UTF-8 characters and are never JSON messages.
class TranscriptionStream {
public:
    static constexpr size_t kMaxTextBytes = 1024;
    void Clear() { id_.clear(); text_.clear(); next_ = 0; }
    void Open(const char* id) {
        Clear();
        if (id && *id && std::char_traits<char>::length(id) <= 128) id_ = id;
    }
    void Append(const char* id, uint64_t index, const uint8_t* data, size_t size) {
        if (!Matches(id)) return;
        if (index != next_ || (size && !data)) { Clear(); return; }
        ++next_;
        const auto available = kMaxTextBytes - text_.size();
        if (size && available) text_.append(reinterpret_cast<const char*>(data),
                                          size < available ? size : available);
    }
    std::string Close(const char* id, bool allowed) {
        if (!Matches(id)) return {};
        std::string result;
        if (allowed) {
            // Only return complete valid UTF-8 scalars (also omit embedded NUL).
            size_t end = 0;
            while (end < text_.size()) {
                const auto c = static_cast<uint8_t>(text_[end]);
                size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                           c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
                if (!c || !n || end + n > text_.size()) break;
                uint32_t scalar = c & (n == 1 ? 0x7f : (1u << (7-n))-1);
                bool valid = true;
                for (size_t j = 1; j < n; ++j) {
                    auto b = static_cast<uint8_t>(text_[end+j]);
                    if ((b & 0xc0) != 0x80) { valid = false; break; }
                    scalar = (scalar << 6) | (b & 0x3f);
                }
                if (!valid || (n == 2 && scalar < 0x80) ||
                    (n == 3 && scalar < 0x800) || (n == 4 && scalar < 0x10000) ||
                    (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff) break;
                end += n;
            }
            result = text_.substr(0, end);
        }
        Clear();
        return result;
    }
private:
    bool Matches(const char* id) const { return !id_.empty() && id && id_ == id; }
    std::string id_, text_;
    uint64_t next_ = 0;
};
} // namespace eidolon
