#include "eidolon/transcription_stream.h"
#include <cassert>
#include <iostream>
using eidolon::TranscriptionStream;
int main() {
    TranscriptionStream s;
    auto append = [&](uint64_t i, const std::string& text) {
        s.Append("a", i, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };
    const std::string chinese = "你好，今天心情不错";
    s.Open("a"); append(0, chinese.substr(0, 2)); append(1, chinese.substr(2));
    assert(s.Close("a", true) == chinese);
    s.Open("a"); append(0, "{plain text, not JSON}");
    assert(s.Close("a", true) == "{plain text, not JSON}");
    s.Open("a"); append(0, "secret"); assert(s.Close("a", false).empty());
    assert(s.Close("a", true).empty());
    s.Open("a"); append(0, "old"); s.Clear(); append(1, "stale");
    assert(s.Close("a", true).empty());
    s.Open("a"); append(1, "missing chunk"); assert(s.Close("a", true).empty());
    s.Open("a"); append(0, "old"); s.Open("b"); append(1, "stale");
    assert(s.Close("a", true).empty()); assert(s.Close("b", true).empty());
    s.Open("a"); append(0, std::string(1023, 'x') + chinese);
    assert(s.Close("a", true) == std::string(1024 - chinese.size(), 'x') + chinese);
    s.Open("a"); append(0, "valid\xed\xa0\x80"); assert(s.Close("a", true) == "valid");
    s.Open("a"); append(0, "valid\xf4\x90\x80\x80"); assert(s.Close("a", true) == "valid");
    s.Open("a"); append(0, std::string("ok\0hidden", 9)); assert(s.Close("a", true) == "ok");
    // Every byte boundary may split a UTF-8 scalar. Publish valid prefixes
    // before Close, keeping the incomplete bytes for the next chunk.
    s.Open("a");
    const std::string spoken = "我叫小何。 Hello!";
    for (size_t i = 0; i < spoken.size(); ++i) {
        append(i, spoken.substr(i, 1));
        const size_t valid = i < 15 ? ((i + 1) / 3) * 3 : i + 1;
        assert(s.Snapshot("a") == spoken.substr(0, valid));
        assert(s.Snapshot("other").empty());
    }
    assert(s.Close("a", true) == spoken);
    assert(s.Snapshot("a").empty());
    s.Open("a");
    for (size_t i = 0; i < 1000; ++i) append(i, "何");
    assert(s.Snapshot("a").size() == 1023);
    append(1000, "结束");
    const auto tail = s.Close("a", true);
    assert(tail.size() <= 1024 && tail.substr(tail.size() - 6) == "结束");
    std::cout << "Transcription stream tests passed\n";
}
