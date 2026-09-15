#include "delivery.h"
#include <algorithm>
#include <cJSON.h>
namespace eidolon::expression {
namespace {
const char* StatusName(Status s) {
    switch(s) {
    case Status::Accepted:return "accepted";
    case Status::Started:return "started";
    case Status::Completed:return "completed";
    case Status::Cancelled:return "cancelled";
    case Status::Rejected:return "rejected";
    default:return "failed";
    }
}
const char* ReasonName(Reason r) {
    switch(r) {
    case Reason::None:return "";
    case Reason::Busy:return "BUSY";
    case Reason::Hidden:return "HIDDEN";
    case Reason::Cancelled:return "CANCELLED";
    case Reason::Superseded:return "SUPERSEDED";
    case Reason::Expired:return "EXPIRED";
    default:return "INVALID_PLAN";
    }
}
}
bool Delivery::Play(const std::string& wire,const std::string& command,Surface& surface,uint64_t now,uint64_t issued) {
    WirePlan parsed;
    if (command.empty() || command.size()>96 || !DecodePlan(wire,1,parsed)) return false;
    for (auto& entry:entries_) if (entry.id==parsed.presentation_id.data()) {
        if (entry.wire!=wire || entry.command!=command) return false;
        if (!entry.receipt.empty()) publish_(entry.command,entry.receipt);
        return true;
    }
    if (issued<=last_issued_ms_) return false;
    auto& entry=entries_[cursor_];
    if (entry.token && entry.token==active_token_) return false;
    last_issued_ms_=issued;
    cursor_=(cursor_+1)%entries_.size();
    entry={};entry.token=next_token_++;entry.start_ms=now;
    entry.id=parsed.presentation_id.data();entry.response=parsed.response_id.data();
    entry.command=command;entry.wire=wire;
    parsed.plan.token=entry.token;
    // Submit's immediate event is handled here; its queued copy is deduplicated.
    Observe(surface.Submit(parsed.plan),now);
    return true;
}
bool Delivery::Cancel(const std::string& id,Surface& surface) {
    for (auto& entry:entries_) if (entry.id==id) {
        if (entry.terminal) { publish_(entry.command,entry.receipt);return true; }
        surface.Cancel(entry.token);return true;
    }
    return false;
}
void Delivery::Observe(Event event,uint64_t now) {
    if (event.status==Status::None) return;
    for (auto& entry:entries_) if (entry.token==event.token) {
        if (entry.terminal || (event.status==Status::Accepted && entry.sequence)) return;
        if (event.status==Status::Started && entry.sequence!=1) return;
        if (event.status==Status::Completed && entry.sequence!=2) return;
        if (event.status==Status::Accepted) active_token_=entry.token;
        if (event.status!=Status::Accepted && event.status!=Status::Started) {
            entry.terminal=true;
            if (active_token_==entry.token) active_token_=0;
        }
        auto* json=cJSON_CreateObject();
        cJSON_AddNumberToObject(json,"schema_version",1);
        cJSON_AddStringToObject(json,"presentation_id",entry.id.c_str());
        cJSON_AddStringToObject(json,"response_id",entry.response.c_str());
        cJSON_AddStringToObject(json,"status",StatusName(event.status));
        cJSON_AddStringToObject(json,"reason",ReasonName(event.reason));
        cJSON_AddNumberToObject(json,"sequence",++entry.sequence);
        cJSON_AddNumberToObject(json,"elapsed_ms",std::min<uint64_t>(now>=entry.start_ms?now-entry.start_ms:0,kMaxDurationMs));
        auto* raw=cJSON_PrintUnformatted(json);
        if (raw) { entry.receipt=raw;cJSON_free(raw);publish_(entry.command,entry.receipt); }
        cJSON_Delete(json);return;
    }
}
void Delivery::Close(Surface& surface) {
    if (active_token_) surface.Cancel(active_token_);
    active_token_=0;last_issued_ms_=0;
    // Pending old UI events carry monotonically unique tokens and cannot bind
    // to the next session's entries. Transport teardown marks them undelivered.
    for (auto& entry:entries_) entry={};
}
}
