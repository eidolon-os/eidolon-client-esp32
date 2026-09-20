#pragma once
#include <functional>
namespace eidolon {
enum class HeadMotionStatus { Accepted, Started, Completed, Cancelled, Rejected, Failed };
struct HeadMotionResult { HeadMotionStatus status; const char* reason; };
using HeadMotionObserver = std::function<void(HeadMotionResult)>;
inline const char* HeadMotionStatusName(HeadMotionStatus s) {
    switch (s) {
    case HeadMotionStatus::Accepted: return "accepted";
    case HeadMotionStatus::Started: return "started";
    case HeadMotionStatus::Completed: return "completed";
    case HeadMotionStatus::Cancelled: return "cancelled";
    case HeadMotionStatus::Rejected: return "rejected";
    default: return "failed";
    }
}
}
