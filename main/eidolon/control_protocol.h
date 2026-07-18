#ifndef EIDOLON_CONTROL_PROTOCOL_H_
#define EIDOLON_CONTROL_PROTOCOL_H_

#include <string>

#include "eidolon_topics.h"

namespace eidolon {

constexpr int kControlProtocolVersion = 1;

struct ControlCommand {
    bool valid = false;
    bool expired = false;
    bool is_v1 = false;
    int capability_version = 0;
    std::string id;
    std::string op;
    std::string payload;
};

ControlCommand ParseControlCommand(const std::string& json);

std::string BuildControlAck(const ControlCommand& command,
                            const std::string& device_id,
                            const std::string& status,
                            const std::string& code,
                            const std::string& message = "",
                            const std::string& result_json = "");

}  // namespace eidolon

#endif  // EIDOLON_CONTROL_PROTOCOL_H_
