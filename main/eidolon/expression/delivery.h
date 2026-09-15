#pragma once
#include <array>
#include <string>
#include "surface.h"
#include "core/wire.h"
namespace eidolon::expression {
// Controller-task owner. Renderer notifications must be queued back to this owner.
// Four recent receipts bound replay memory. An issuance watermark rejects old
// commands even after eviction; disconnect clears session authority.
class Delivery {
public:
    using Publish = std::function<void(const std::string& command_id, const std::string& receipt)>;
    explicit Delivery(Publish publish):publish_(std::move(publish)) {}
    bool Play(const std::string& plan_json, const std::string& command_id, Surface& surface, uint64_t now_ms, uint64_t issued_ms);
    bool Cancel(const std::string& id, Surface& surface);
    void Observe(Event event, uint64_t now_ms);
    void Close(Surface& surface);
private:
    struct Entry {
        uint64_t token=0, start_ms=0;
        uint32_t sequence=0;
        std::string id, response, command, wire, receipt;
        bool terminal=false;
    };
    std::array<Entry,4> entries_{};
    size_t cursor_=0;
    uint64_t next_token_=1,active_token_=0,last_issued_ms_=0;
    Publish publish_;
};
}
