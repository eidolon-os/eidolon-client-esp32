#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace eidolon {
// Data and authenticated participant signalling arrive on different tasks.
// Hold at most one small lifecycle packet until signalling identifies its sender.
// This never grants authority: the caller must supply the server's Agent verdict.
class PendingSessionControl {
public:
    bool Stage(const std::string& sender, const std::string& payload,
               uint32_t generation, int64_t now_ms) {
        if (sender.empty() || sender.size()>128 || payload.empty() || payload.size()>1024)
            return false;
        if (!payload_.empty() && now_ms<expires_ms_) return false;
        sender_=sender;payload_=payload;generation_=generation;expires_ms_=now_ms+1000;
        return true;
    }
    std::string Resolve(const std::string& sender, bool agent,
                        uint32_t generation, int64_t now_ms) {
        if (payload_.empty()) return {};
        if (generation!=generation_ || now_ms>=expires_ms_) { Clear();return {}; }
        if (sender!=sender_) return {};
        std::string result;
        if (agent) result=std::move(payload_);
        Clear();return result;
    }
    void Clear() { sender_.clear();payload_.clear();generation_=0;expires_ms_=0; }
private:
    std::string sender_,payload_;
    uint32_t generation_=0;
    int64_t expires_ms_=0;
};
}
