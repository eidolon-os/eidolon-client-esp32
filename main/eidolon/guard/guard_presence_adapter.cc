#include "guard/guard_presence_adapter.h"

#include "eidolon_topics.h"

#include <cstdio>
#include <utility>

namespace eidolon {
namespace {

std::string JsonEscape(const std::string& value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
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

}  // namespace

void GuardPresenceAdapter::Configure(GuardPresenceRuntime runtime)
{
    runtime_ = std::move(runtime);
    candidate_emitted_ = false;
    absence_emitted_ = false;
}

void GuardPresenceAdapter::Clear()
{
    runtime_ = GuardPresenceRuntime{};
    candidate_emitted_ = false;
    absence_emitted_ = false;
}

std::optional<std::string> GuardPresenceAdapter::Build(const GuardObservation& observation,
                                                        uint64_t ts_ms)
{
    if (!runtime_.usable()) {
        return std::nullopt;
    }

    const std::string companion_id = JsonEscape(runtime_.guard_companion_id);
    const std::string device_id = JsonEscape(runtime_.device_id);
    const std::string correlation_id = JsonEscape(CorrelationId(observation.epoch));
    const std::string prefix =
        std::string("{\"schema_v\":1,\"guard_companion_id\":\"") + companion_id +
        "\",\"device_id\":\"" + device_id + "\",\"correlation_id\":\"" +
        correlation_id + "\",\"guard_epoch\":" + std::to_string(observation.epoch) +
        ",\"ts_ms\":" + std::to_string(ts_ms);

    if (observation.state == GuardState::Candidate) {
        if (candidate_emitted_ && candidate_epoch_ == observation.epoch) {
            return std::nullopt;
        }
        candidate_emitted_ = true;
        candidate_epoch_ = observation.epoch;
        return std::string("{\"type\":\"") + kGuardPresenceCandidateType + "\"," +
               prefix.substr(1) +
               ",\"signals\":{\"observation_sequence\":" +
               std::to_string(observation.sequence) + ",\"runtime_revision\":" +
               std::to_string(runtime_.runtime_revision) + "},\"camera\":{\"motion_score\":" +
               std::to_string(observation.motion_score) +
               "},\"raw_retention\":\"none\",\"debounce_ms\":" +
               std::to_string(runtime_.candidate_debounce_ms) + "}";
    }

    if (observation.state == GuardState::Absent) {
        if (absence_emitted_ && absence_epoch_ == observation.epoch) {
            return std::nullopt;
        }
        absence_emitted_ = true;
        absence_epoch_ = observation.epoch;
        const uint64_t absent_for_ms = observation.now_ms >= observation.last_seen_ms
                                           ? observation.now_ms - observation.last_seen_ms
                                           : 0;
        return std::string("{\"type\":\"") + kGuardPresenceAbsentType + "\"," +
               prefix.substr(1) + ",\"reason\":\"timeout\",\"absent_for_ms\":" +
               std::to_string(absent_for_ms) + ",\"raw_retention\":\"none\"}";
    }

    return std::nullopt;
}

std::string GuardPresenceAdapter::CorrelationId(uint32_t guard_epoch) const
{
    char value[48] = {};
    std::snprintf(value, sizeof(value), "g-%08lx-r%lu-e%lu",
                  static_cast<unsigned long>(runtime_.boot_nonce),
                  static_cast<unsigned long>(runtime_.runtime_revision),
                  static_cast<unsigned long>(guard_epoch));
    return value;
}

}  // namespace eidolon
