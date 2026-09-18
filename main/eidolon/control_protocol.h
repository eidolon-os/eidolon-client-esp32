#ifndef EIDOLON_CONTROL_PROTOCOL_H_
#define EIDOLON_CONTROL_PROTOCOL_H_

#include <string>
#include <cstdint>

#include "eidolon_topics.h"

namespace eidolon {

constexpr int kControlProtocolVersion = 1;

struct ControlCommand {
    bool valid = false;
    bool expired = false;
    bool is_v1 = false;
    bool bounded_deadline = false;
    bool clock_known = false;
    uint64_t issued_ms = 0;
    int capability_version = 0;
    std::string id;
    std::string op;
    std::string payload;
};

// Caller supplies the current configuration's trusted UTC reading. Zero means
// unknown; never infer freshness from a command's own timestamp or wall clock.
ControlCommand ParseControlCommand(const std::string& json, int64_t now_utc_ms);

std::string BuildControlAck(const ControlCommand& command,
                            const std::string& device_id,
                            const std::string& status,
                            const std::string& code,
                            const std::string& message = "",
                            const std::string& result_json = "",
                            int64_t now_utc_ms = 0);

}  // namespace eidolon

#endif  // EIDOLON_CONTROL_PROTOCOL_H_
