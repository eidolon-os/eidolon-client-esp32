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
    assert(s.Close("a", true) == std::string(1023, 'x'));
    s.Open("a"); append(0, "valid\xed\xa0\x80"); assert(s.Close("a", true) == "valid");
    s.Open("a"); append(0, "valid\xf4\x90\x80\x80"); assert(s.Close("a", true) == "valid");
    s.Open("a"); append(0, std::string("ok\0hidden", 9)); assert(s.Close("a", true) == "ok");
    std::cout << "Transcription stream tests passed\n";
}
